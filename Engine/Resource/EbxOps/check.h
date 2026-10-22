#pragma once

#include "util.h"

#include <string>
#include <vector>

namespace tool {

struct CheckResult {
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    std::string summary;
    [[nodiscard]] bool ok() const { return errors.empty(); }
};

// Structural validation of written EBX bytes: re-read with ReSkate's reader, every fixup entry resolved,
// array counts, record hashes in the RFL2 group table, boxed references, type-reference collisions
// (ReSkate's repair_type_fixups must find nothing), exported instance order.
[[nodiscard]] CheckResult check_ebx(std::span<const std::byte> bytes);

} // namespace tool
