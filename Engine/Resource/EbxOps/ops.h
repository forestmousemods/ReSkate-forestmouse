#pragma once

#include "rawimage.h"

#include <string>
#include <vector>

namespace tool {

struct ApplyResult {
    std::vector<std::string> lines;   // what each op did
    bool sorted{};                    // exported instances were put in guid order
    std::vector<std::size_t> moveTo;  // pre-sort index -> output index
};

// Applies a JSON op list (see NOTES.md) to the document. Throws with the failing op's number on error.
ApplyResult apply_ops(ebx::Document& document, EditState& state, const std::string& opsJson);

} // namespace tool
