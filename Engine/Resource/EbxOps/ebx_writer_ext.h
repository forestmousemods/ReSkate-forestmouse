#pragma once

// Our patched copy of ReSkate's EBX writer (src/ebx_writer_ext.cpp, from Engine/Resource/ebx_writer.cpp).
// The original stays available as reskate_write_document (compiled from the fork under another name).

#include "ebx_document.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <vector>

namespace dingosdk::frostbite::ebx {

struct WriteOptions {
    bool refuseNewArrayHashes{};
    // EFIX's trailing boxed-reference list, by the boxed record a reference points at (old-image offsets,
    // i.e. BoxedValueRecord::offset / BoxedReference::dataOffset). Every reference (instance-level or
    // inside a carried array) whose record is in the set is listed. nullopt = ReSkate's original rule
    // (old and new instance-level slots matched by their sorted index).
    std::optional<std::set<std::uint32_t>> listedBoxedRecords;
};

struct WriteReport {
    std::size_t carriedArrays{};
    std::size_t carriedBoxedReferences{};   // boxed references found inside carried arrays / boxed values
    std::vector<std::uint32_t> unknownTargets; // old pointer slots inside carried data whose target was not understood
};

[[nodiscard]] std::vector<std::byte> write_document_ext(const Document& document, const WriteOptions& options,
                                                        WriteReport* report = nullptr);

// ReSkate's untouched writer (Engine/Resource/ebx_writer.cpp compiled with renamed symbols).
[[nodiscard]] std::vector<std::byte> reskate_write_document(const Document& document, bool refuseNewArrayHashes);

} // namespace dingosdk::frostbite::ebx
