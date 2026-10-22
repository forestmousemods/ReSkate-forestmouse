#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

// Our fork: a mod's EBX edits as an op list (the ebxtool format, own_slot_build/ebxtool NOTES.md: clone instances,
// set fields by path, append to arrays - inside boxed values too), applied to another copy of the asset. The mod merge
// uses it for an asset several mods change that the EBX merge cannot combine (the wardrobe UI, whose lists live in
// boxed values): the edits of a mod that ships them (Mods/<mod>/reskate-ops) go onto the other mods' copy.
namespace dingosdk::frostbite::ebx_ops {

struct Applied {
    std::vector<std::byte> bytes;     // the edited document (empty when it failed)
    std::vector<std::string> lines;   // what each op did
    std::string error;                // why it failed
};

// Never throws. The result has been read back, checked and written again byte for byte.
[[nodiscard]] Applied apply(std::span<const std::byte> ebx, const std::string& opsJson);

} // namespace dingosdk::frostbite::ebx_ops
