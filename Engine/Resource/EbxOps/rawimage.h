#pragma once

// Editing the "old image" of a Document: the EBXD bytes it was read from (Document::payload) plus the fixup
// tables and EBXX records that describe them. ReSkate's writer lays instances out again from their parsed
// objects, but carries boxed values (and the arrays / strings / boxed values they reach) from this image by
// old offset. Structural edits inside boxed values are therefore edits of this image, kept consistent:
//  - insert_gap: open n bytes at old offset P; every offset at or after P moves (pointer displacements,
//    every fixup table, array / boxed records, section offsets, instance offsets, BoxedReference targets).
//  - new arrays and new strings are appended past the end of the image (the writer classifies a target by
//    array record first, and anything else from the string section on as a string);
//  - new boxed records are opened at the end of the boxed section (before the strings);
//  - an instance added by an edit has no bytes in the image: it gets a virtual offset far past the end, so a
//    carried pointer can name it (the writer resolves carried pointers to instances by old offset).

#include "util.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace tool {

struct EditState {
    std::set<std::uint32_t> listed;           // boxed records whose reference is in EFIX's trailing list
    std::uint32_t nextVirtual{0x40000000};    // virtual old offsets for instances added by edits
    std::map<std::string, std::size_t> names; // "$name" -> instance index (pre-sort numbering)
    std::vector<std::string> log;
};

// The listed set of a document as read: EFIX trailing-list slots -> the boxed record each one points at.
[[nodiscard]] std::set<std::uint32_t> listed_records_from(const ebx::Document& document);
[[nodiscard]] EditState make_state(const ebx::Document& document);

enum class TargetKind { null, instance, array, boxed, string, empty, unknown };
struct Target {
    TargetKind kind{TargetKind::unknown};
    std::size_t index{};      // instance index / array record index / boxed record index
    std::uint32_t offset{};   // the old-image offset
};
[[nodiscard]] Target classify(const ebx::Document& document, std::uint32_t target);
[[nodiscard]] std::string target_kind_name(TargetKind kind);

// Displacement stored at a relative-pointer slot of the image (handles i64 boxed halves and the
// 0xffffffff "no string" sentinel).
struct Relative {
    std::int64_t target{};
    bool wide{};      // stored as i64 with a sign-extended upper half
    bool sentinel{};  // 0xffffffff
    bool inconsistent{}; // upper half neither 0 nor the sign extension of the lower (a broken i64)
};
[[nodiscard]] Relative read_relative(const ebx::Document& document, std::uint32_t slot);
void write_relative(ebx::Document& document, std::uint32_t slot, std::int64_t target, bool wide = false);

void insert_gap(ebx::Document& document, EditState& state, std::uint32_t at, std::uint32_t size);
// BoxedValueRecord::rawBytes again from the image (offset to the next record / the strings).
void reslice_boxed(ebx::Document& document);

// The type of what an array record or a boxed record holds.
struct RawType {
    ebx::FieldType type{};
    bool array{};          // the content is an array slot (8-byte pointer to an array)
    int typeIndex{-1};     // reflection type for structures
    std::uint16_t rawEncoded{};
    std::uint16_t classRef{0xffff};
};
[[nodiscard]] RawType record_type(const ebx::Document& document, std::uint16_t rawEncoded, std::uint16_t classRef);
[[nodiscard]] std::size_t value_size(const ebx::Document& document, ebx::FieldType type, int typeIndex);
[[nodiscard]] std::size_t value_alignment(const ebx::Document& document, ebx::FieldType type, int typeIndex);

[[nodiscard]] std::optional<std::size_t> boxed_record_at(const ebx::Document& document, std::uint32_t offset);
[[nodiscard]] std::optional<std::size_t> array_record_at(const ebx::Document& document, std::uint32_t offset);

// Deep copies inside the image: the copy gets its own boxed records and arrays; pointers to instances and
// strings keep their targets. Return the new record's index.
std::size_t clone_boxed(ebx::Document& document, EditState& state, std::size_t boxedIndex);
std::size_t clone_array(ebx::Document& document, EditState& state, std::size_t arrayIndex);
// Grows an array record by one element at its end (in place). With cloneFrom >= 0 the element is a deep copy
// of that element, otherwise zeroes. Returns the new element's old offset.
std::uint32_t array_append(ebx::Document& document, EditState& state, std::size_t arrayIndex, int cloneFrom);
// The raw array's last element moved to index `to`; the elements from `to` on move one on, each with its relocations
// (pointers re-encoded for their new slots).
void array_move_last(ebx::Document& document, EditState& state, std::size_t arrayIndex, std::uint32_t to);

// Raw stores (fixup tables kept consistent).
void set_pointer_null(ebx::Document& document, std::uint32_t slot);
void set_pointer_import(ebx::Document& document, std::uint32_t slot, std::size_t importIndex);
void set_pointer_instance(ebx::Document& document, std::uint32_t slot, std::size_t instanceIndex);
void set_cstring(ebx::Document& document, std::uint32_t slot, const std::string& text);
[[nodiscard]] std::string read_cstring(const ebx::Document& document, std::uint32_t slot);

// Deep copies of parsed values (boxed references get their own boxed records).
[[nodiscard]] ebx::Value deep_clone_value(ebx::Document& document, EditState& state, const ebx::Value& value);
[[nodiscard]] std::shared_ptr<ebx::Object> deep_clone_object(ebx::Document& document, EditState& state,
                                                             const ebx::Object& object);
// Appends a deep copy of an instance (new guid unless given). Returns its index.
std::size_t clone_instance(ebx::Document& document, EditState& state, std::size_t source,
                           std::optional<fb::Guid> guid, std::optional<bool> exported);

// Import index for (file, instance), added when missing.
std::size_t ensure_import(ebx::Document& document, const fb::Guid& file, const fb::Guid& instance);

// Puts the instances in the order the game expects: root, exported ascending by stored guid (when the
// document was sorted that way), internal last. Remaps every parsed pointer. Returns old -> new index.
std::vector<std::size_t> order_instances(ebx::Document& document, bool sortExported);
[[nodiscard]] bool exported_sorted(const ebx::Document& document);

} // namespace tool
