#pragma once

#include "ebx_document.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fb = dingosdk::frostbite;
namespace ebx = dingosdk::frostbite::ebx;

namespace tool {

[[nodiscard]] std::vector<std::byte> read_file(const std::filesystem::path& path);
void write_file(const std::filesystem::path& path, std::span<const std::byte> bytes);
void write_text(const std::filesystem::path& path, std::string_view text);

[[nodiscard]] std::string hex(std::span<const std::byte> bytes);
[[nodiscard]] std::string hex32(std::uint64_t value);   // 0x-prefixed lowercase
[[nodiscard]] std::string guid_text(const fb::Guid& guid); // canonical text (as Studio and ReSkate print it)
[[nodiscard]] fb::Guid parse_guid(std::string_view text);  // inverse of guid_text
[[nodiscard]] bool looks_like_guid(std::string_view text);
[[nodiscard]] fb::Guid random_guid();
[[nodiscard]] std::uint32_t djb2(std::string_view text);  // h = h*33 ^ c, seed 5381 (the game's slot hash)

[[nodiscard]] std::string field_type_name(ebx::FieldType type);
[[nodiscard]] std::string category_name(ebx::FieldCategory category);
[[nodiscard]] std::size_t scalar_size(ebx::FieldType type);

// The reflection type a fixup slot (EFIX type table index) names: matched by guid (and signature when the
// slot has one). -1 when no reflection type matches (array / pointer helper types have no descriptor).
[[nodiscard]] int type_for_fixup_slot(const ebx::Document& document, std::size_t slot);
[[nodiscard]] std::string fixup_slot_label(const ebx::Document& document, std::size_t slot);
[[nodiscard]] std::string type_name(const ebx::Document& document, int descriptor);

// Field descriptors of a type, inherited ones first (the order the reader fills Object::fields).
[[nodiscard]] std::vector<std::size_t> flat_fields(const ebx::Document& document, std::size_t typeIndex);
// A field by name (case-insensitive) among a type's flattened fields; nullopt when absent.
[[nodiscard]] std::optional<std::size_t> find_field(const ebx::Document& document, std::size_t typeIndex,
                                                    std::string_view name);
// Enum member name for a value, when the enum type carries its members in the reflection.
[[nodiscard]] std::optional<std::string> enum_name(const ebx::Document& document, std::size_t enumType,
                                                   std::int64_t value);
[[nodiscard]] std::optional<std::int64_t> enum_value(const ebx::Document& document, std::size_t enumType,
                                                     std::string_view name);

// payload helpers (old image)
template <typename T> T load(const std::vector<std::byte>& bytes, std::size_t at) {
    if (at + sizeof(T) > bytes.size()) throw std::out_of_range("read past the EBXD image");
    T value{};
    std::memcpy(&value, bytes.data() + at, sizeof(T));
    return value;
}
template <typename T> void store(std::vector<std::byte>& bytes, std::size_t at, T value) {
    if (at + sizeof(T) > bytes.size()) throw std::out_of_range("write past the EBXD image");
    std::memcpy(bytes.data() + at, &value, sizeof(T));
}
[[nodiscard]] inline std::size_t align_up(std::size_t value, std::size_t alignment) {
    if (alignment <= 1) return value;
    return (value + alignment - 1) / alignment * alignment;
}

[[nodiscard]] std::string lower(std::string_view text);

} // namespace tool
