#include "rawimage.h"

#include <algorithm>
#include <array>
#include <functional>
#include <unordered_set>

namespace tool {

using ebx::BoxedReference;
using ebx::Document;
using ebx::FieldType;
using ebx::Object;
using ebx::PointerKind;
using ebx::PointerReference;
using ebx::Value;

namespace {

void erase_value(std::vector<std::uint32_t>& values, std::uint32_t value) {
    std::erase(values, value);
}

void add_unique(std::vector<std::uint32_t>& values, std::uint32_t value) {
    if (std::ranges::find(values, value) == values.end()) values.push_back(value);
}

std::vector<std::uint32_t> in_range(const std::vector<std::uint32_t>& values, std::uint32_t start, std::size_t length) {
    std::vector<std::uint32_t> out;
    for (const auto v : values)
        if (v >= start && v - start < length) out.push_back(v);
    std::ranges::sort(out);
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// Every parsed value of every instance, each object once.
void each_value(Document& document, const std::function<void(Value&)>& visit) {
    std::unordered_set<const Object*> seen;
    std::function<void(Value&)> walk = [&](Value& value) {
        visit(value);
        if (auto* nested = std::get_if<std::shared_ptr<Object>>(&value.data)) {
            if (*nested && seen.insert(nested->get()).second)
                for (auto& field : (*nested)->fields) walk(field.value);
        } else if (auto* array = std::get_if<Value::Array>(&value.data)) {
            for (auto& element : *array) walk(element);
        }
    };
    for (auto& instance : document.instances)
        if (instance.object && seen.insert(instance.object.get()).second)
            for (auto& field : instance.object->fields) walk(field.value);
}

void sort_boxed(Document& document) {
    std::ranges::stable_sort(document.boxedValues, {}, &ebx::BoxedValueRecord::offset);
}

} // namespace

std::set<std::uint32_t> listed_records_from(const Document& document) {
    std::set<std::uint32_t> out;
    for (const auto slot : document.boxedReferenceOffsets) {
        if (static_cast<std::size_t>(slot) + 16 > document.payload.size()) continue;
        const auto displacement = load<std::int64_t>(document.payload, slot + 8);
        const auto target = static_cast<std::int64_t>(slot) + 8 + displacement;
        if (target >= 0 && target <= static_cast<std::int64_t>(document.payload.size()))
            out.insert(static_cast<std::uint32_t>(target));
    }
    return out;
}

EditState make_state(const Document& document) {
    EditState state;
    state.listed = listed_records_from(document);
    state.nextVirtual = std::max<std::uint32_t>(0x40000000U,
        static_cast<std::uint32_t>(align_up(document.payload.size(), 16) + 0x01000000U));
    return state;
}

std::optional<std::size_t> boxed_record_at(const Document& document, std::uint32_t offset) {
    for (std::size_t i = 0; i < document.boxedValues.size(); ++i)
        if (document.boxedValues[i].offset == offset) return i;
    return std::nullopt;
}

std::optional<std::size_t> array_record_at(const Document& document, std::uint32_t offset) {
    for (std::size_t i = 0; i < document.arrays.size(); ++i)
        if (document.arrays[i].offset == offset) return i;
    return std::nullopt;
}

Target classify(const Document& document, std::uint32_t target) {
    // Same order as the writer's old_target: array, string, instance, boxed, empty.
    if (const auto a = array_record_at(document, target)) return {TargetKind::array, *a, target};
    if (document.stringsOffset <= target && target < document.payload.size()) return {TargetKind::string, 0, target};
    for (std::size_t i = 0; i < document.instances.size(); ++i)
        if (document.instances[i].dataOffset == target) return {TargetKind::instance, i, target};
    if (const auto b = boxed_record_at(document, target)) return {TargetKind::boxed, *b, target};
    if (target >= 4 && target <= document.payload.size() && load<std::uint32_t>(document.payload, target - 4) == 0)
        return {TargetKind::empty, 0, target};
    return {TargetKind::unknown, 0, target};
}

std::string target_kind_name(TargetKind kind) {
    switch (kind) {
    case TargetKind::null: return "null";
    case TargetKind::instance: return "instance";
    case TargetKind::array: return "array";
    case TargetKind::boxed: return "boxed";
    case TargetKind::string: return "string";
    case TargetKind::empty: return "empty-array";
    case TargetKind::unknown: return "unknown";
    }
    return "?";
}

Relative read_relative(const Document& document, std::uint32_t slot) {
    Relative out;
    const auto low = load<std::uint32_t>(document.payload, slot);
    if (low == 0xffffffffU) {
        out.sentinel = true;
        return out;
    }
    const auto d32 = static_cast<std::int32_t>(low);
    std::uint32_t high = 0;
    if (static_cast<std::size_t>(slot) + 8 <= document.payload.size()) high = load<std::uint32_t>(document.payload, slot + 4);
    // A 32-bit displacement keeps its upper half 0 (even when negative: the writer stores pointers that way);
    // an i64 one (boxed reference halves) is sign-extended. Anything else is reported as inconsistent.
    out.wide = high == 0xffffffffU;
    out.inconsistent = (high == 0xffffffffU && d32 >= 0) || (high != 0 && high != 0xffffffffU);
    out.target = out.wide ? static_cast<std::int64_t>(slot) + load<std::int64_t>(document.payload, slot)
                          : static_cast<std::int64_t>(slot) + d32;
    return out;
}

void write_relative(Document& document, std::uint32_t slot, std::int64_t target, bool wide) {
    const auto displacement = target - static_cast<std::int64_t>(slot);
    if (wide) {
        store<std::int64_t>(document.payload, slot, displacement);
    } else {
        if (displacement < INT32_MIN || displacement > INT32_MAX) throw std::overflow_error("relative pointer out of range");
        store<std::int32_t>(document.payload, slot, static_cast<std::int32_t>(displacement));
        if (static_cast<std::size_t>(slot) + 8 <= document.payload.size()) store<std::uint32_t>(document.payload, slot + 4, 0);
    }
}

void reslice_boxed(Document& document) {
    sort_boxed(document);
    for (std::size_t i = 0; i < document.boxedValues.size(); ++i) {
        auto& record = document.boxedValues[i];
        const auto end = i + 1 < document.boxedValues.size() ? document.boxedValues[i + 1].offset : document.stringsOffset;
        if (end < record.offset || end > document.payload.size()) throw std::runtime_error("boxed records out of order");
        record.rawBytes.assign(document.payload.begin() + record.offset, document.payload.begin() + end);
    }
}

void insert_gap(Document& document, EditState& state, std::uint32_t at, std::uint32_t size) {
    if (size == 0) return;
    if (at > document.payload.size()) throw std::runtime_error("gap past the end of the image");
    const auto shift = [&](std::uint64_t x) -> std::uint32_t {
        return static_cast<std::uint32_t>(x >= at ? x + size : x);
    };
    struct Fix {
        std::uint32_t slot;
        Relative relative;
    };
    std::vector<Fix> fixes;
    fixes.reserve(document.pointerOffsets.size());
    for (const auto slot : document.pointerOffsets)
        if (static_cast<std::size_t>(slot) + 4 <= document.payload.size()) fixes.push_back({slot, read_relative(document, slot)});

    document.payload.insert(document.payload.begin() + at, size, std::byte{0});

    for (auto* table : {&document.pointerOffsets, &document.resourceRefOffsets, &document.importOffsets,
                        &document.typeInfoOffsets, &document.boxedReferenceOffsets, &document.boxedValueSlots})
        for (auto& v : *table) v = shift(v);
    for (auto& instance : document.instances) instance.dataOffset = shift(instance.dataOffset);
    for (auto& record : document.arrays) record.offset = shift(record.offset);
    for (auto& record : document.boxedValues) record.offset = shift(record.offset);
    document.arraysOffset = shift(document.arraysOffset);
    document.boxedValuesOffset = shift(document.boxedValuesOffset);
    document.stringsOffset = shift(document.stringsOffset);
    std::set<std::uint32_t> listed;
    for (const auto v : state.listed) listed.insert(shift(v));
    state.listed = std::move(listed);
    state.nextVirtual = shift(state.nextVirtual);
    each_value(document, [&](Value& value) {
        if (auto* boxed = std::get_if<BoxedReference>(&value.data); boxed && boxed->dataOffset >= 0)
            boxed->dataOffset = shift(static_cast<std::uint64_t>(boxed->dataOffset));
    });

    std::unordered_set<std::uint32_t> boxedStarts;
    for (const auto& record : document.boxedValues) boxedStarts.insert(record.offset);
    for (const auto& fix : fixes) {
        if (fix.relative.sentinel) continue;
        const auto slot = shift(fix.slot);
        const auto target = fix.relative.target >= 0 ? static_cast<std::int64_t>(shift(static_cast<std::uint64_t>(fix.relative.target)))
                                                     : fix.relative.target;
        const bool wide = fix.relative.wide || (target >= 0 && boxedStarts.contains(static_cast<std::uint32_t>(target)));
        write_relative(document, slot, target, wide);
    }
    reslice_boxed(document);
}

RawType record_type(const Document& document, std::uint16_t rawEncoded, std::uint16_t classRef) {
    ebx::FieldDescriptor probe;
    probe.encodedType = static_cast<std::uint16_t>(rawEncoded >> 1);
    RawType out;
    out.type = probe.type();
    out.array = probe.category() == ebx::FieldCategory::array;
    out.rawEncoded = rawEncoded;
    out.classRef = classRef;
    if (out.type == FieldType::structure || out.type == FieldType::enumeration)
        out.typeIndex = type_for_fixup_slot(document, classRef);
    return out;
}

std::size_t value_size(const Document& document, FieldType type, int typeIndex) {
    if (type == FieldType::structure) {
        if (typeIndex < 0) throw std::runtime_error("struct of unknown type");
        return std::max<std::size_t>(document.types[static_cast<std::size_t>(typeIndex)].size, 1);
    }
    return scalar_size(type);
}

std::size_t value_alignment(const Document& document, FieldType type, int typeIndex) {
    if (type == FieldType::structure) {
        if (typeIndex < 0) throw std::runtime_error("struct of unknown type");
        return std::max<std::size_t>(document.types[static_cast<std::size_t>(typeIndex)].alignment, 1);
    }
    return std::max<std::size_t>(std::min<std::size_t>(scalar_size(type), 8), 1);
}

namespace {

std::size_t record_content_size(const Document& document, const RawType& type) {
    return type.array ? 8 : value_size(document, type.type, type.typeIndex);
}

// Copies the fixup entries of [from, from + length) to [to, to + length). Pointer slots are re-aimed:
// arrays and boxed records they reach are deep-copied (deep == true), anything else keeps its target.
// The source and destination are given as functions of the current image, because the deep copies open
// gaps that may move either of them.
void copy_relocations(Document& document, EditState& state, const std::function<std::uint32_t()>& from,
                      const std::function<std::uint32_t()>& to, std::size_t length, bool deep) {
    const auto source = from();
    const auto destination = to();
    for (auto* table : {&document.resourceRefOffsets, &document.importOffsets, &document.typeInfoOffsets})
        for (const auto v : in_range(*table, source, length)) add_unique(*table, destination + (v - source));
    std::vector<std::uint32_t> relative;
    for (const auto v : in_range(document.pointerOffsets, source, length)) relative.push_back(v - source);
    for (const auto r : relative) {
        const auto sourceSlot = from() + r;
        const auto rel = read_relative(document, sourceSlot);
        if (rel.sentinel) {
            const auto slot = to() + r;
            store<std::uint32_t>(document.payload, slot, 0xffffffffU);
            add_unique(document.pointerOffsets, slot);
            continue;
        }
        const auto target = classify(document, static_cast<std::uint32_t>(rel.target));
        std::int64_t newTarget = rel.target;
        bool wide = rel.wide;
        if (deep && target.kind == TargetKind::array) {
            const auto copy = clone_array(document, state, target.index);
            newTarget = document.arrays[copy].offset;
        } else if (deep && target.kind == TargetKind::boxed) {
            const auto copy = clone_boxed(document, state, target.index);
            newTarget = document.boxedValues[copy].offset;
            wide = true;
        } else if (target.kind == TargetKind::boxed) {
            wide = true;
        }
        const auto slot = to() + r;
        write_relative(document, slot, newTarget, wide);
        add_unique(document.pointerOffsets, slot);
    }
}

} // namespace

std::size_t clone_boxed(Document& document, EditState& state, std::size_t boxedIndex) {
    const auto record = document.boxedValues.at(boxedIndex);   // copy: the vector changes below
    const auto type = record_type(document, record.encodedType, record.classRef);
    const auto size = record_content_size(document, type);
    const auto alignment = std::max<std::size_t>(8, type.array ? 8 : value_alignment(document, type.type, type.typeIndex));
    const auto at = document.stringsOffset;
    const auto start = static_cast<std::uint32_t>(document.boxedValuesOffset +
                                                  align_up(at - document.boxedValuesOffset, alignment));
    const auto gap = static_cast<std::uint32_t>(align_up((start - at) + size, 16));
    insert_gap(document, state, at, gap);
    // The source lies before the gap, so it did not move.
    std::copy_n(document.payload.begin() + record.offset, size, document.payload.begin() + start);
    ebx::BoxedValueRecord copy;
    copy.offset = start;
    copy.count = record.count;
    copy.hash = record.hash;
    copy.encodedType = record.encodedType;
    copy.classRef = record.classRef;
    document.boxedValues.push_back(copy);
    sort_boxed(document);
    if (state.listed.contains(record.offset)) state.listed.insert(start);
    // Nested copies open their gaps at the (new) end of the boxed section, after both ends: they stay put.
    const auto sourceOffset = record.offset;
    copy_relocations(document, state, [sourceOffset]() { return sourceOffset; }, [start]() { return start; }, size, true);
    reslice_boxed(document);
    const auto index = boxed_record_at(document, start);
    if (!index) throw std::runtime_error("cloned boxed record went missing");
    state.log.push_back("boxed record @" + std::to_string(sourceOffset) + " copied to @" + std::to_string(start));
    return *index;
}

std::size_t clone_array(Document& document, EditState& state, std::size_t arrayIndex) {
    const auto record = document.arrays.at(arrayIndex);
    const auto type = record_type(document, record.encodedType, record.classRef);
    const auto stride = value_size(document, type.type, type.typeIndex);
    const auto alignment = std::max<std::size_t>(4, value_alignment(document, type.type, type.typeIndex));
    const auto length = stride * record.count;
    const auto start = document.payload.size();
    const auto data = align_up(start + 4, alignment);
    document.payload.resize(align_up(data + length, 16), std::byte{0});
    store<std::uint32_t>(document.payload, data - 4, record.count);
    std::copy_n(document.payload.begin() + record.offset, length, document.payload.begin() + static_cast<std::ptrdiff_t>(data));
    ebx::ArrayRecord copy = record;
    copy.offset = static_cast<std::uint32_t>(data);
    document.arrays.push_back(copy);
    const auto newIndex = document.arrays.size() - 1;
    // A nested boxed copy opens a gap before the strings, which moves this new array (it lies past the
    // strings), and moves the source too when the source is itself past the strings.
    auto findSource = [&document, arrayIndex]() { return document.arrays[arrayIndex].offset; };
    auto findCopy = [&document, newIndex]() { return document.arrays[newIndex].offset; };
    copy_relocations(document, state, findSource, findCopy, length, true);
    state.log.push_back("array @" + std::to_string(record.offset) + " copied to @" + std::to_string(document.arrays[newIndex].offset));
    return newIndex;
}

std::uint32_t array_append(Document& document, EditState& state, std::size_t arrayIndex, int cloneFrom) {
    const auto record = document.arrays.at(arrayIndex);
    const auto type = record_type(document, record.encodedType, record.classRef);
    const auto stride = static_cast<std::uint32_t>(value_size(document, type.type, type.typeIndex));
    if (record.count == 0) throw std::runtime_error("cannot grow an empty raw array in place (it has no storage)");
    if (cloneFrom >= static_cast<int>(record.count)) throw std::runtime_error("clone_of element is out of range");
    const auto at = record.offset + record.count * stride;
    insert_gap(document, state, at, static_cast<std::uint32_t>(align_up(stride, 16)));
    // The array starts before the gap: it did not move; the new element is the gap's start. Positions are
    // taken from the record each time (a nested boxed copy may move an array that lies past the strings).
    const auto count = record.count;
    store<std::uint32_t>(document.payload, record.offset - 4, count + 1);
    document.arrays[arrayIndex].count = count + 1;
    if (cloneFrom >= 0) {
        const auto source = record.offset + static_cast<std::uint32_t>(cloneFrom) * stride;
        std::copy_n(document.payload.begin() + source, stride, document.payload.begin() + at);
        const auto from = static_cast<std::uint32_t>(cloneFrom);
        copy_relocations(document, state,
                         [&document, arrayIndex, from, stride]() { return document.arrays[arrayIndex].offset + from * stride; },
                         [&document, arrayIndex, count, stride]() { return document.arrays[arrayIndex].offset + count * stride; },
                         stride, true);
    }
    reslice_boxed(document);
    state.log.push_back("array @" + std::to_string(document.arrays[arrayIndex].offset) + " grown to " + std::to_string(count + 1));
    return document.arrays[arrayIndex].offset + count * stride;
}

void array_move_last(Document& document, EditState& state, std::size_t arrayIndex, std::uint32_t to) {
    const auto record = document.arrays.at(arrayIndex);
    const auto type = record_type(document, record.encodedType, record.classRef);
    const auto stride = static_cast<std::uint32_t>(value_size(document, type.type, type.typeIndex));
    if (record.count == 0 || to >= record.count) throw std::runtime_error("at: index out of range");
    const auto last = record.count - 1;
    if (to == last) return;
    const auto at = [&document, arrayIndex, stride](std::uint32_t i) { return document.arrays[arrayIndex].offset + i * stride; };
    // the last element, kept aside: its bytes and the relocations in it
    std::vector<std::byte> bytes(document.payload.begin() + at(last), document.payload.begin() + at(last) + stride);
    struct KeptSlot { std::vector<std::uint32_t>* table; std::uint32_t r; };
    std::vector<KeptSlot> slots;
    for (auto* table : {&document.resourceRefOffsets, &document.importOffsets, &document.typeInfoOffsets})
        for (const auto v : in_range(*table, at(last), stride)) slots.push_back({table, v - at(last)});
    struct KeptPointer { std::uint32_t r; Relative rel; };
    std::vector<KeptPointer> pointers;
    for (const auto v : in_range(document.pointerOffsets, at(last), stride)) pointers.push_back({v - at(last), read_relative(document, v)});
    const auto clear = [&document, stride](std::uint32_t start) {
        for (auto* table : {&document.resourceRefOffsets, &document.importOffsets, &document.typeInfoOffsets, &document.pointerOffsets})
            for (const auto v : in_range(*table, start, stride)) erase_value(*table, v);
    };
    for (auto i = last; i > to; --i) {
        clear(at(i));
        std::copy_n(document.payload.begin() + at(i - 1), stride, document.payload.begin() + at(i));
        copy_relocations(document, state, [&at, i]() { return at(i - 1); }, [&at, i]() { return at(i); }, stride, false);
    }
    clear(at(to));
    std::copy(bytes.begin(), bytes.end(), document.payload.begin() + at(to));
    for (const auto& k : slots) add_unique(*k.table, at(to) + k.r);
    for (const auto& p : pointers) {
        const auto slot = at(to) + p.r;
        if (p.rel.sentinel) {
            store<std::uint32_t>(document.payload, slot, 0xffffffffU);
        } else {
            const auto target = classify(document, static_cast<std::uint32_t>(p.rel.target));
            write_relative(document, slot, p.rel.target, p.rel.wide || target.kind == TargetKind::boxed);
        }
        add_unique(document.pointerOffsets, slot);
    }
    state.log.push_back("array @" + std::to_string(document.arrays[arrayIndex].offset) + ": its last element moved to " +
                        std::to_string(to));
}

void set_pointer_null(Document& document, std::uint32_t slot) {
    erase_value(document.pointerOffsets, slot);
    erase_value(document.importOffsets, slot);
    store<std::uint64_t>(document.payload, slot, 0);
}

void set_pointer_import(Document& document, std::uint32_t slot, std::size_t importIndex) {
    if (importIndex >= document.imports.size()) throw std::runtime_error("import index out of range");
    erase_value(document.pointerOffsets, slot);
    store<std::uint64_t>(document.payload, slot, (static_cast<std::uint64_t>(importIndex) << 1) | 1U);
    add_unique(document.importOffsets, slot);
}

void set_pointer_instance(Document& document, std::uint32_t slot, std::size_t instanceIndex) {
    if (instanceIndex >= document.instances.size()) throw std::runtime_error("instance index out of range");
    erase_value(document.importOffsets, slot);
    store<std::uint64_t>(document.payload, slot, 0);
    write_relative(document, slot, document.instances[instanceIndex].dataOffset, false);
    add_unique(document.pointerOffsets, slot);
}

void set_cstring(Document& document, std::uint32_t slot, const std::string& text) {
    // A new string past the end of the image: the writer takes any target from the string section on as a
    // string and adds it to its own table.
    const auto at = static_cast<std::uint32_t>(document.payload.size());
    document.payload.insert(document.payload.end(), reinterpret_cast<const std::byte*>(text.data()),
                            reinterpret_cast<const std::byte*>(text.data()) + text.size());
    document.payload.push_back(std::byte{0});
    erase_value(document.importOffsets, slot);
    store<std::uint64_t>(document.payload, slot, 0);
    write_relative(document, slot, at, false);
    add_unique(document.pointerOffsets, slot);
}

std::string read_cstring(const Document& document, std::uint32_t slot) {
    const auto rel = read_relative(document, slot);
    if (rel.sentinel) return {};
    if (rel.target < 0 || static_cast<std::size_t>(rel.target) >= document.payload.size()) throw std::runtime_error("string target out of range");
    std::string out;
    for (auto at = static_cast<std::size_t>(rel.target); at < document.payload.size() && document.payload[at] != std::byte{0}; ++at)
        out.push_back(static_cast<char>(document.payload[at]));
    return out;
}

Value deep_clone_value(Document& document, EditState& state, const Value& value) {
    Value out;
    out.arrayHash = value.arrayHash;
    if (const auto* boxed = std::get_if<BoxedReference>(&value.data)) {
        auto copy = *boxed;
        if (boxed->encodedType != 0 && boxed->dataOffset >= 0) {
            if (const auto index = boxed_record_at(document, static_cast<std::uint32_t>(boxed->dataOffset))) {
                const auto made = clone_boxed(document, state, *index);
                copy.dataOffset = document.boxedValues[made].offset;
                copy.relativeOffset = 0;
            }
        }
        out.data = copy;
        return out;
    }
    if (const auto* nested = std::get_if<std::shared_ptr<Object>>(&value.data)) {
        out.data = *nested ? deep_clone_object(document, state, **nested) : nullptr;
        return out;
    }
    if (const auto* array = std::get_if<Value::Array>(&value.data)) {
        Value::Array copy;
        copy.reserve(array->size());
        for (const auto& element : *array) copy.push_back(deep_clone_value(document, state, element));
        out.data = std::move(copy);
        return out;
    }
    out.data = value.data;
    return out;
}

std::shared_ptr<Object> deep_clone_object(Document& document, EditState& state, const Object& object) {
    auto out = std::make_shared<Object>();
    out->descriptor = object.descriptor;
    out->fields.reserve(object.fields.size());
    for (const auto& field : object.fields)
        out->fields.push_back({field.descriptor, field.name, deep_clone_value(document, state, field.value)});
    return out;
}

namespace {

std::uint32_t object_id(const fb::Guid& guid) {
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(guid.bytes[0])) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(guid.bytes[1])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(guid.bytes[2])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(guid.bytes[3])) << 24U);
}

// ReSkate's ebx_merge.cpp renumber(): an exported instance's Flags may hold its id (guid[0..3] & 0x01ffffff);
// a clone re-derives it from its own guid.
bool renumber_flags(Object& object, const fb::Guid& donor, const fb::Guid& guid) {
    constexpr std::uint32_t mask = 0x01ffffffU;
    for (auto& field : object.fields) {
        if (field.name != "Flags") continue;
        auto* s = std::get_if<std::int64_t>(&field.value.data);
        auto* u = std::get_if<std::uint64_t>(&field.value.data);
        if (!s && !u) return false;
        const auto current = s ? static_cast<std::uint32_t>(*s) : static_cast<std::uint32_t>(*u);
        if ((current & mask) != (object_id(donor) & mask)) return false;
        const auto renumbered = (current & ~mask) | (object_id(guid) & mask);
        if (s) *s = static_cast<std::int32_t>(renumbered);
        else *u = renumbered;
        return true;
    }
    return false;
}

} // namespace

std::size_t clone_instance(Document& document, EditState& state, std::size_t source,
                           std::optional<fb::Guid> guid, std::optional<bool> exported) {
    if (source >= document.instances.size()) throw std::runtime_error("clone source index out of range");
    if (!document.instances[source].object) throw std::runtime_error("clone source has no parsed object");
    ebx::InstanceRecord copy;
    {
        const auto& from = document.instances[source];
        copy.fixupType = from.fixupType;
        copy.descriptor = from.descriptor;
        copy.exported = exported.value_or(from.exported);
        copy.rawImage = from.rawImage;
        copy.instanceGuid = copy.exported ? guid.value_or(random_guid()) : fb::Guid{};
        if (!copy.exported && guid) copy.instanceGuid = *guid;
    }
    const auto donorGuid = document.instances[source].instanceGuid;
    const auto donorExported = document.instances[source].exported;
    // The DataContainer header (bytes 16..24 of every instance) differs by kind in all 2385 instances of the 329
    // game files seen: exported 02 00 00 00 00 b1 00 00, internal 02 00 00 00 00 a0 00 00. A clone that changes
    // kind takes the header of an instance of its new kind (or that pattern).
    if (copy.exported != donorExported && copy.rawImage.size() >= 24) {
        std::array<std::byte, 8> header{std::byte{2}, {}, {}, {}, {}, std::byte{static_cast<unsigned char>(copy.exported ? 0xb1 : 0xa0)}, {}, {}};
        for (const auto& other : document.instances)
            if (other.exported == copy.exported && other.rawImage.size() >= 24) {
                std::copy_n(other.rawImage.begin() + 16, 8, header.begin());
                break;
            }
        std::copy_n(header.begin(), 8, copy.rawImage.begin() + 16);
        state.log.push_back("instance header set for the clone's new kind");
    }
    // The object is cloned before the record joins the document (its boxed copies open gaps).
    auto object = deep_clone_object(document, state, *document.instances[source].object);
    copy.object = object;
    copy.dataOffset = state.nextVirtual;
    state.nextVirtual += 0x100;
    if (copy.exported && donorExported && renumber_flags(*copy.object, donorGuid, copy.instanceGuid))
        state.log.push_back("Flags re-derived from the clone's guid");
    if (copy.exported)
        for (const auto& existing : document.instances)
            if (existing.exported && existing.instanceGuid == copy.instanceGuid)
                throw std::runtime_error("an exported instance already has guid " + guid_text(copy.instanceGuid));
    document.instances.push_back(std::move(copy));
    return document.instances.size() - 1;
}

std::size_t ensure_import(Document& document, const fb::Guid& file, const fb::Guid& instance) {
    for (std::size_t i = 0; i < document.imports.size(); ++i)
        if (document.imports[i].fileGuid == file && document.imports[i].classGuid == instance) return i;
    document.imports.push_back({file, instance});
    return document.imports.size() - 1;
}

bool exported_sorted(const Document& document) {
    std::size_t last = 0;
    for (std::size_t i = 1; i < document.instances.size(); ++i) {
        if (!document.instances[i].exported) break;
        if (last && std::memcmp(document.instances[last].instanceGuid.bytes.data(),
                                document.instances[i].instanceGuid.bytes.data(), 16) > 0)
            return false;
        last = i;
    }
    return true;
}

std::vector<std::size_t> order_instances(Document& document, bool sortExported) {
    const auto count = document.instances.size();
    std::vector<std::size_t> exported, internal;
    for (std::size_t i = 1; i < count; ++i) (document.instances[i].exported ? exported : internal).push_back(i);
    if (sortExported)
        std::stable_sort(exported.begin(), exported.end(), [&](std::size_t l, std::size_t r) {
            return std::memcmp(document.instances[l].instanceGuid.bytes.data(),
                               document.instances[r].instanceGuid.bytes.data(), 16) < 0;
        });
    std::vector<std::size_t> order;
    if (count) order.push_back(0);
    order.insert(order.end(), exported.begin(), exported.end());
    order.insert(order.end(), internal.begin(), internal.end());
    std::vector<std::size_t> moveTo(count);
    for (std::size_t i = 0; i < order.size(); ++i) moveTo[order[i]] = i;
    bool moved = false;
    for (std::size_t i = 0; i < order.size(); ++i) moved |= order[i] != i;
    if (!moved) return moveTo;
    std::vector<ebx::InstanceRecord> sorted;
    sorted.reserve(count);
    for (const auto from : order) sorted.push_back(std::move(document.instances[from]));
    document.instances = std::move(sorted);
    each_value(document, [&](Value& value) {
        if (auto* pointer = std::get_if<PointerReference>(&value.data);
            pointer && pointer->kind == PointerKind::internal && pointer->index >= 0 &&
            static_cast<std::size_t>(pointer->index) < moveTo.size())
            pointer->index = static_cast<std::int32_t>(moveTo[static_cast<std::size_t>(pointer->index)]);
    });
    return moveTo;
}

} // namespace tool
