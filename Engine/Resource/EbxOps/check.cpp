#include "check.h"

#include "ebx_fixup_repair.h"
#include "rawimage.h"

#include <map>
#include <unordered_set>

namespace tool {

CheckResult check_ebx(std::span<const std::byte> bytes) {
    CheckResult out;
    ebx::Document d;
    try {
        d = ebx::read_document(bytes);
    } catch (const std::exception& error) {
        out.errors.push_back(std::string("ReSkate's reader refuses it: ") + error.what());
        out.summary = "unreadable";
        return out;
    }
    try {
        if (const auto repair = ebx::repair_type_fixups(bytes))
            out.errors.push_back(std::to_string(repair->removed) + " type-reference fixups collide with other fixups (repair_type_fixups)");
    } catch (const std::exception& error) {
        out.errors.push_back(std::string("repair_type_fixups threw: ") + error.what());
    }

    std::unordered_set<std::uint32_t> groupKeys;
    for (const auto& g : d.reflectionGroups) groupKeys.insert(g.hash);
    const bool haveGroups = d.hashedReflectionNames;

    std::map<TargetKind, std::size_t> kinds;
    std::map<std::uint32_t, std::size_t> boxedRefs;   // record offset -> references
    for (const auto slot : d.pointerOffsets) {
        const auto rel = read_relative(d, slot);
        if (rel.sentinel) { ++kinds[TargetKind::null]; continue; }
        if (rel.inconsistent)
            out.errors.push_back("pointer slot " + std::to_string(slot) + " has an inconsistent upper half (broken 64-bit displacement)");
        const auto target = rel.target >= 0 ? classify(d, static_cast<std::uint32_t>(rel.target)) : Target{};
        ++kinds[target.kind];
        if (target.kind == TargetKind::unknown)
            out.errors.push_back("pointer slot " + std::to_string(slot) + " -> " + std::to_string(rel.target) + " resolves to nothing");
        if (target.kind == TargetKind::boxed) ++boxedRefs[target.offset];
    }
    const std::unordered_set<std::uint32_t> pointerSet(d.pointerOffsets.begin(), d.pointerOffsets.end());
    for (const auto slot : d.importOffsets) {
        const auto v = load<std::uint64_t>(d.payload, slot);
        if (!(v & 1U) || (v >> 1) >= d.imports.size())
            out.errors.push_back("import slot " + std::to_string(slot) + " holds " + hex32(v) + " (no import)");
        if (pointerSet.contains(slot)) out.errors.push_back("slot " + std::to_string(slot) + " is both a pointer and an import");
    }
    for (const auto& a : d.arrays) {
        if (a.offset < 4) { out.errors.push_back("array record at offset < 4"); continue; }
        if (load<std::uint32_t>(d.payload, a.offset - 4) != a.count)
            out.errors.push_back("array @" + std::to_string(a.offset) + " record count " + std::to_string(a.count) +
                                 " != stored " + std::to_string(load<std::uint32_t>(d.payload, a.offset - 4)));
        if (haveGroups && !groupKeys.contains(a.hash)) out.errors.push_back("array @" + std::to_string(a.offset) + " hash " + hex32(a.hash) + " not in RFL2 groups");
    }
    for (const auto& b : d.boxedValues) {
        if (haveGroups && !groupKeys.contains(b.hash)) out.errors.push_back("boxed @" + std::to_string(b.offset) + " hash " + hex32(b.hash) + " not in RFL2 groups");
        if (b.offset < d.boxedValuesOffset || b.offset >= d.stringsOffset) out.errors.push_back("boxed @" + std::to_string(b.offset) + " outside the boxed section");
    }
    // Instance-level boxed references (the reader recorded their slots).
    for (const auto slot : d.boxedValueSlots) {
        const auto encoded = load<std::uint32_t>(d.payload, slot);
        if (encoded == 0) continue;
        if (!pointerSet.contains(slot + 8)) out.errors.push_back("boxed reference " + std::to_string(slot) + " has no pointer fixup");
    }
    for (const auto& b : d.boxedValues) {
        const auto n = boxedRefs.contains(b.offset) ? boxedRefs[b.offset] : 0;
        if (n != 1) out.warnings.push_back("boxed record @" + std::to_string(b.offset) + " is referenced " + std::to_string(n) + " times");
    }
    for (const auto slot : d.boxedReferenceOffsets) {
        if (load<std::uint32_t>(d.payload, slot) == 0 || !pointerSet.contains(slot + 8)) {
            out.errors.push_back("trailing boxed-reference list names " + std::to_string(slot) + ", which is no boxed reference");
            continue;
        }
        const auto rel = read_relative(d, slot + 8);
        if (classify(d, static_cast<std::uint32_t>(rel.target)).kind != TargetKind::boxed)
            out.errors.push_back("trailing boxed-reference " + std::to_string(slot) + " points at no boxed record");
    }
    // The trailing list against the rule every listed reference of 329 game files follows (NOTES.md): a boxed
    // pointer to an instance (or null), a boxed pointer array that is empty or holds instance pointers, and
    // DataDefinedStructValue / DataDefinedEnumValue (alone or as arrays) are listed; nothing else is.
    {
        const std::unordered_set<std::uint32_t> listedSlots(d.boxedReferenceOffsets.begin(), d.boxedReferenceOffsets.end());
        std::size_t mismatched = 0;
        for (const auto slot : d.typeInfoOffsets) {
            if (!pointerSet.contains(slot + 8)) continue;
            const auto rel = read_relative(d, slot + 8);
            if (rel.target < 0) continue;
            const auto target = classify(d, static_cast<std::uint32_t>(rel.target));
            if (target.kind != TargetKind::boxed) continue;
            const auto& record = d.boxedValues[target.index];
            const auto rt = record_type(d, record.encodedType, record.classRef);
            bool expected = false;
            const auto pointerKind = [&](std::uint32_t at) {
                const auto v = load<std::uint64_t>(d.payload, at);
                return v == 0 ? 0 : (v & 1U) ? 1 : 2;   // null / import / internal
            };
            if (rt.type == ebx::FieldType::pointer && !rt.array) {
                expected = pointerKind(record.offset) != 1;
            } else if (rt.type == ebx::FieldType::pointer && rt.array) {
                const auto arrayRel = read_relative(d, record.offset);
                const auto array = arrayRel.target >= 0 ? array_record_at(d, static_cast<std::uint32_t>(arrayRel.target)) : std::nullopt;
                if (!array) expected = true;   // empty
                else
                    for (std::uint32_t k = 0; k < d.arrays[*array].count; ++k)
                        expected |= pointerKind(d.arrays[*array].offset + 8 * k) == 2;
            } else if (rt.type == ebx::FieldType::structure) {
                const auto name = type_name(d, rt.typeIndex);
                expected = name == "DataDefinedStructValue" || name == "DataDefinedEnumValue";
            }
            if (expected != listedSlots.contains(slot)) ++mismatched;
        }
        if (mismatched)
            out.warnings.push_back(std::to_string(mismatched) + " boxed references are (not) listed against the rule seen in game files");
    }
    if (!exported_sorted(d)) out.warnings.push_back("exported instances are not in guid order");
    std::unordered_set<std::string> guids;
    for (const auto& inst : d.instances)
        if (inst.exported && !guids.insert(guid_text(inst.instanceGuid)).second)
            out.errors.push_back("duplicate exported guid " + guid_text(inst.instanceGuid));
    std::size_t headers = 0;
    for (const auto& inst : d.instances) {
        if (inst.descriptor < 0) out.errors.push_back("an instance's type is not in the reflection table");
        // DataContainer header by kind, as in all 2385 instances of the game files seen (NOTES.md).
        if (inst.rawImage.size() >= 24 &&
            (std::to_integer<unsigned>(inst.rawImage[16]) != 2 ||
             std::to_integer<unsigned>(inst.rawImage[21]) != (inst.exported ? 0xb1U : 0xa0U)))
            ++headers;
    }
    if (headers) out.warnings.push_back(std::to_string(headers) + " instance headers (bytes 16..24) do not match their exported/internal kind");

    out.summary = std::to_string(d.instances.size()) + " instances, " + std::to_string(d.imports.size()) + " imports, " +
                  std::to_string(d.arrays.size()) + " arrays, " + std::to_string(d.boxedValues.size()) + " boxed, pointers: " +
                  std::to_string(kinds[TargetKind::instance]) + " instance / " + std::to_string(kinds[TargetKind::array]) + " array / " +
                  std::to_string(kinds[TargetKind::empty]) + " empty / " + std::to_string(kinds[TargetKind::string]) + " string / " +
                  std::to_string(kinds[TargetKind::boxed]) + " boxed, " + std::to_string(d.importOffsets.size()) + " import slots, " +
                  std::to_string(d.typeInfoOffsets.size()) + " type-info slots, " + std::to_string(d.boxedReferenceOffsets.size()) + " listed boxed refs";
    return out;
}

} // namespace tool
