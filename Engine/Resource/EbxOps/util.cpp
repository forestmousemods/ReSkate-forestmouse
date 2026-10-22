#include "util.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cctype>
#include <fstream>
#include <random>

namespace tool {

std::vector<std::byte> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    in.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(in.tellg());
    in.seekg(0);
    std::vector<std::byte> bytes(size);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (!in) throw std::runtime_error("cannot read " + path.string());
    return bytes;
}

void write_file(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write " + path.string());
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("cannot write " + path.string());
}

void write_text(const std::filesystem::path& path, std::string_view text) {
    write_file(path, std::as_bytes(std::span(text.data(), text.size())));
}

std::string hex(std::span<const std::byte> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const auto b : bytes) {
        const auto v = std::to_integer<unsigned>(b);
        out.push_back(digits[v >> 4]);
        out.push_back(digits[v & 15]);
    }
    return out;
}

std::string hex32(std::uint64_t value) {
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "0x%llx", static_cast<unsigned long long>(value));
    return buffer;
}

std::string guid_text(const fb::Guid& guid) { return guid.string(); }

bool looks_like_guid(std::string_view text) {
    if (text.size() != 36) return false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (text[i] != '-') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(text[i]))) {
            return false;
        }
    }
    return true;
}

fb::Guid parse_guid(std::string_view text) {
    if (!looks_like_guid(text)) throw std::runtime_error("not a guid: " + std::string(text));
    std::array<std::byte, 16> canonical{};
    std::size_t k = 0;
    auto nibble = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
        return static_cast<unsigned>(std::tolower(static_cast<unsigned char>(c)) - 'a' + 10);
    };
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '-') { ++i; continue; }
        canonical[k++] = static_cast<std::byte>((nibble(text[i]) << 4) | nibble(text[i + 1]));
        i += 2;
    }
    // Inverse of Guid::string(): the first three fields are stored swapped.
    fb::Guid g;
    g.bytes = {canonical[3], canonical[2], canonical[1], canonical[0], canonical[5], canonical[4], canonical[7],
               canonical[6], canonical[8], canonical[9], canonical[10], canonical[11], canonical[12], canonical[13],
               canonical[14], canonical[15]};
    return g;
}

fb::Guid random_guid() {
    static std::random_device device;
    fb::Guid g;
    for (auto& b : g.bytes) b = static_cast<std::byte>(device() & 0xff);
    // RFC 4122 version 4 / variant bits on the canonical form (bytes 7 and 8 of the stored order).
    g.bytes[7] = static_cast<std::byte>((std::to_integer<unsigned>(g.bytes[7]) & 0x0f) | 0x40);
    g.bytes[8] = static_cast<std::byte>((std::to_integer<unsigned>(g.bytes[8]) & 0x3f) | 0x80);
    return g;
}

std::uint32_t djb2(std::string_view text) {
    std::uint32_t h = 5381;
    for (const auto c : text) h = (h * 33U) ^ static_cast<unsigned char>(c);
    return h;
}

std::string field_type_name(ebx::FieldType type) {
    using T = ebx::FieldType;
    switch (type) {
    case T::inherited: return "inherited";
    case T::dbObject: return "dbObject";
    case T::structure: return "struct";
    case T::pointer: return "pointer";
    case T::array: return "array";
    case T::fixedArray: return "fixedArray";
    case T::string: return "string32";
    case T::cString: return "cstring";
    case T::enumeration: return "enum";
    case T::fileRef: return "fileRef";
    case T::boolean: return "bool";
    case T::int8: return "int8";
    case T::uint8: return "uint8";
    case T::int16: return "int16";
    case T::uint16: return "uint16";
    case T::int32: return "int32";
    case T::uint32: return "uint32";
    case T::uint64: return "uint64";
    case T::int64: return "int64";
    case T::float32: return "float32";
    case T::float64: return "float64";
    case T::guid: return "guid";
    case T::sha1: return "sha1";
    case T::resourceRef: return "resourceRef";
    case T::function: return "function";
    case T::typeRef: return "typeRef";
    case T::boxedValueRef: return "boxedValueRef";
    case T::interfaceType: return "interface";
    case T::delegate: return "delegate";
    }
    return "type" + std::to_string(static_cast<int>(type));
}

std::string category_name(ebx::FieldCategory category) {
    using C = ebx::FieldCategory;
    switch (category) {
    case C::none: return "none";
    case C::pointer: return "pointer";
    case C::structure: return "struct";
    case C::primitive: return "primitive";
    case C::array: return "array";
    case C::enumeration: return "enum";
    case C::function: return "function";
    case C::interfaceType: return "interface";
    case C::delegate: return "delegate";
    }
    return "category" + std::to_string(static_cast<int>(category));
}

std::size_t scalar_size(ebx::FieldType type) {
    using T = ebx::FieldType;
    switch (type) {
    case T::boolean:
    case T::int8:
    case T::uint8: return 1;
    case T::int16:
    case T::uint16: return 2;
    case T::int32:
    case T::uint32:
    case T::float32:
    case T::enumeration: return 4;
    case T::guid: return 16;
    case T::sha1: return 20;
    case T::string: return 32;
    case T::boxedValueRef: return 16;
    default: return 8;
    }
}

int type_for_fixup_slot(const ebx::Document& document, std::size_t slot) {
    if (slot >= document.fixupTypeGuids.size()) return -1;
    const auto& guid = document.fixupTypeGuids[slot];
    const bool hasSignature = slot < document.fixupTypeSignatures.size();
    for (std::size_t i = 0; i < document.types.size(); ++i) {
        const auto& type = document.types[i];
        if (type.guid != guid) continue;
        if (hasSignature && type.signature != document.fixupTypeSignatures[slot]) continue;
        return static_cast<int>(i);
    }
    return -1;
}

std::string type_name(const ebx::Document& document, int descriptor) {
    if (descriptor < 0 || static_cast<std::size_t>(descriptor) >= document.types.size()) return {};
    return document.types[static_cast<std::size_t>(descriptor)].name;
}

std::string fixup_slot_label(const ebx::Document& document, std::size_t slot) {
    const auto t = type_for_fixup_slot(document, slot);
    if (t >= 0) return type_name(document, t);
    if (slot >= document.fixupTypeGuids.size()) return "slot" + std::to_string(slot);
    const auto& guid = document.fixupTypeGuids[slot];
    // Observed: an array type's guid is its element type's guid with the last byte + 1.
    for (const auto& type : document.types) {
        if (type.guid == fb::Guid{}) continue;
        auto next = type.guid;
        next.bytes[15] = static_cast<std::byte>(std::to_integer<unsigned>(next.bytes[15]) + 1);
        if (next == guid) return type.name + "[] (guid " + guid_text(guid) + ")";
    }
    // A data-defined type: the guid is the instance guid of the type asset an import names.
    for (std::size_t i = 0; i < document.imports.size(); ++i)
        if (document.imports[i].classGuid == guid) return "data-defined type of import " + std::to_string(i) + " (guid " + guid_text(guid) + ")";
    return "guid:" + guid_text(guid);
}

static void flatten(const ebx::Document& document, std::size_t typeIndex, std::vector<std::size_t>& out, int depth) {
    if (depth > 64) throw std::runtime_error("type nesting too deep");
    const auto& type = document.types.at(typeIndex);
    for (std::size_t k = 0; k < type.fieldCount; ++k) {
        const auto index = static_cast<std::size_t>(type.fieldIndex) + k;
        const auto& field = document.fields.at(index);
        if (field.type() == ebx::FieldType::inherited) {
            flatten(document, field.classRef, out, depth + 1);
            continue;
        }
        out.push_back(index);
    }
}

std::vector<std::size_t> flat_fields(const ebx::Document& document, std::size_t typeIndex) {
    std::vector<std::size_t> out;
    flatten(document, typeIndex, out, 0);
    return out;
}

std::string lower(std::string_view text) {
    std::string out(text);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::optional<std::size_t> find_field(const ebx::Document& document, std::size_t typeIndex, std::string_view name) {
    const auto wanted = lower(name);
    for (const auto index : flat_fields(document, typeIndex))
        if (lower(document.fields[index].name) == wanted) return index;
    return std::nullopt;
}

std::optional<std::string> enum_name(const ebx::Document& document, std::size_t enumType, std::int64_t value) {
    if (enumType >= document.types.size()) return std::nullopt;
    const auto& type = document.types[enumType];
    if (type.type() != ebx::FieldType::enumeration) return std::nullopt;
    for (std::size_t k = 0; k < type.fieldCount; ++k) {
        const auto& member = document.fields.at(static_cast<std::size_t>(type.fieldIndex) + k);
        if (static_cast<std::int32_t>(member.dataOffset) == static_cast<std::int32_t>(value)) return member.name;
    }
    return std::nullopt;
}

std::optional<std::int64_t> enum_value(const ebx::Document& document, std::size_t enumType, std::string_view name) {
    if (enumType >= document.types.size()) return std::nullopt;
    const auto& type = document.types[enumType];
    if (type.type() != ebx::FieldType::enumeration) return std::nullopt;
    const auto wanted = lower(name);
    for (std::size_t k = 0; k < type.fieldCount; ++k) {
        const auto& member = document.fields.at(static_cast<std::size_t>(type.fieldIndex) + k);
        const auto full = lower(member.name);
        // members are named "<Enum>_<Member>"; accept either form
        if (full == wanted || (full.size() > wanted.size() && full.ends_with("_" + wanted)))
            return static_cast<std::int32_t>(member.dataOffset);
    }
    return std::nullopt;
}

} // namespace tool
