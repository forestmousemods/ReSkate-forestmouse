#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dingosdk::mods::detail {

// A Lua script resource (resource type 0xEC383B87, characters/lua/skaterlualoaderasset and the like): u64 name offset
// (0x20), u64 source offset, u64 relocation table offset, u32 0, u32 source length; the name, NUL, the source, NUL,
// 8-aligned table of three u32 (0, 8, 16): the three offsets above. Its resource meta: u32 table offset, u32 12, 8 zero
// bytes. Only scripts laid out exactly so are read (others are left to the usual rule: the highest mod's wins).
struct LuaScript {
    std::string name;     // "Source/Characters/Lua/SkaterLoader.lua"
    std::string source;
};
[[nodiscard]] std::optional<LuaScript> read_lua_script(std::span<const std::byte> resource);
[[nodiscard]] std::vector<std::byte> write_lua_script(const LuaScript& script);
[[nodiscard]] std::array<std::byte, 16> lua_script_meta(std::span<const std::byte> resource);

// Two mods' versions of one game script, combined line by line against the game's (a three-way merge): what only
// one of them changed is taken from it, what both changed alike once. Lines both changed differently are a
// conflict: no result. `higher` is the higher-priority mod's version.
[[nodiscard]] std::optional<std::string> merge_lua_source(const std::string& game, const std::string& higher,
                                                          const std::string& lower);
// The same on whole resources (read, merged, written again).
[[nodiscard]] std::optional<std::vector<std::byte>> merge_lua_script(std::span<const std::byte> game,
                                                                     std::span<const std::byte> higher,
                                                                     std::span<const std::byte> lower);

} // namespace dingosdk::mods::detail
