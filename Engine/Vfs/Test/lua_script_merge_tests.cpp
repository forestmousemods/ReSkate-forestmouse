// Offline tests of the Lua script merge (lua_script_merge.h). With the paths of the game's skater loader resource and
// three sources (ReSkate Studio's "material targets v1" version, ours on the game's, ours on Studio's) it also merges
// the real scripts:
//   lua_script_merge_tests <game.res> <game.meta> <studio_v1.lua> <ours_on_game.lua> <ours_on_studio.lua>
#include "Engine/Vfs/lua_script_merge.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace dingosdk::mods::detail;
namespace {
int failures = 0, checks = 0;
void expect(bool ok, const char* what) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL %s\n", what); }
    else std::printf("ok   %s\n", what);
}
std::string read_text(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
std::vector<std::byte> read_bytes(const char* path) {
    const auto text = read_text(path);
    std::vector<std::byte> out(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) out[i] = static_cast<std::byte>(text[i]);
    return out;
}
} // namespace

int main(int argc, char** argv) {
    // Small cases.
    const std::string base = "a\r\nb\r\nc\r\nd\r\ne\r\n";
    expect(merge_lua_source(base, "a\r\nB\r\nc\r\nd\r\ne\r\n", "a\r\nb\r\nc\r\nd\r\nE\r\n") == "a\r\nB\r\nc\r\nd\r\nE\r\n",
           "two changes in different places both kept");
    expect(merge_lua_source(base, "a\r\nx\r\nb\r\nc\r\nd\r\ne\r\n", "a\r\nb\r\nc\r\nd\r\ny\r\ne\r\n") ==
               "a\r\nx\r\nb\r\nc\r\nd\r\ny\r\ne\r\n", "two insertions in different places both kept");
    expect(!merge_lua_source(base, "a\r\nB1\r\nc\r\nd\r\ne\r\n", "a\r\nB2\r\nc\r\nd\r\ne\r\n"), "the same line changed two ways: no merge");
    expect(merge_lua_source(base, "a\r\nB\r\nc\r\nd\r\ne\r\n", "a\r\nB\r\nc\r\nd\r\ne\r\n") == "a\r\nB\r\nc\r\nd\r\ne\r\n",
           "the same change twice: once");
    expect(merge_lua_source(base, base, "a\r\nc\r\nd\r\ne\r\n") == "a\r\nc\r\nd\r\ne\r\n", "a deletion kept");
    expect(merge_lua_source(base, "a\r\nb\r\nc\r\nd\r\ne\r\nf", base) == "a\r\nb\r\nc\r\nd\r\ne\r\nf", "an unterminated last line");
    expect(merge_lua_source(base, "a\r\nB\r\nc\r\nd\r\ne\r\n", "a\nb\nc\nd\nE\n") == "a\r\nB\r\nc\r\nd\r\nE\r\n",
           "a version with LF line ends merges with CRLF ones (Skaterize's loader copy)");
    expect(merge_lua_source(base, "a\nb\nX\nd\ne\n", base) == "a\r\nb\r\nX\r\nd\r\ne\r\n", "an LF higher version too");
    const LuaScript script{"Source/Test.lua", "print(1)\r\n"};
    const auto written = write_lua_script(script);
    const auto back = read_lua_script(written);
    expect(back && back->name == script.name && back->source == script.source, "a written script reads back");
    if (argc == 6) {
        const auto gameRes = read_bytes(argv[1]);
        const auto gameMeta = read_bytes(argv[2]);
        const auto game = read_lua_script(gameRes);
        expect(game.has_value(), "the game's skater loader reads");
        if (game) {
            expect(write_lua_script(*game) == gameRes, "the game's skater loader is written back byte for byte");
            const auto meta = lua_script_meta(gameRes);
            expect(std::vector<std::byte>(meta.begin(), meta.end()) == gameMeta, "its resource meta is made again exactly");
            const auto studio = read_text(argv[3]), oursOnGame = read_text(argv[4]), oursOnStudio = read_text(argv[5]);
            const auto merged = merge_lua_source(game->source, studio, oursOnGame);
            expect(merged == oursOnStudio, "Studio's v1 (higher) + ours on the game's = ours on Studio's");
            const auto swapped = merge_lua_source(game->source, oursOnGame, studio);
            expect(swapped == oursOnStudio, "the same with the priorities the other way");
            expect(merge_lua_source(game->source, studio, oursOnStudio) == oursOnStudio, "Studio's v1 + ours on Studio's = ours");
            expect(merge_lua_source(game->source, oursOnStudio, studio) == oursOnStudio, "ours on Studio's + Studio's v1 = ours");
            const auto viaResources = merge_lua_script(gameRes, write_lua_script({game->name, studio}), write_lua_script({game->name, oursOnGame}));
            expect(viaResources && read_lua_script(*viaResources) && read_lua_script(*viaResources)->source == oursOnStudio,
                   "the same through whole resources");
            std::string clash = studio;
            const auto at = clash.find("FullbodyTop_mat");
            if (at != std::string::npos) clash.replace(at, 15, "FullbodyTip_mat");
            std::string other = oursOnGame;
            const auto at2 = other.find("FullbodyTop_mat");
            if (at2 != std::string::npos) other.replace(at2, 15, "FullbodyTap_mat");
            expect(!merge_lua_source(game->source, clash, other), "a real clash is refused");
        }
    }
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
