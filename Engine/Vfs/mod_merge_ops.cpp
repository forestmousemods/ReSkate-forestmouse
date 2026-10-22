#include "mod_merge_internal.h"
#include "Engine/Core/Platform/path_text.h"
#include "Engine/Resource/cas_codec.h"
#include "Engine/Resource/EbxOps/ebx_ops_apply.h"

#include <algorithm>
#include <exception>
#include <map>
#include <set>

// Our fork: a mod's edits to a game asset as an op list (2026-10-19: Physical Lanyards' Accessories tab beside Kori's
// Skaterize, which both change the wardrobe UI). The EBX merge combines added instances, root-array appends and plain
// values; the wardrobe's lists live in boxed values, so of two mods' copies one was kept whole and the other mod's
// tabs were gone. A mod that ships its edits as ops (Mods/<mod>/reskate-ops/<asset name, "/" as "~">.json, the
// ebxtool format) has them made again on the copy of the highest mod without ops for that asset, and that version goes
// into every copy (mod_merge_combine.cpp, `reconciled`). Where no other mod changes the asset, the mod's own copy
// stands as it is.
namespace dingosdk::mods::detail {

std::vector<AssetOps> read_asset_ops(const Mod& mod, MergeReport& report) {
    std::vector<AssetOps> out;
    const auto folder = mod.directory / L"reskate-ops";
    std::error_code error;
    if (!fs::is_directory(folder, error)) return out;
    std::vector<fs::path> files;
    for (fs::directory_iterator it(folder, error), end; it != end && !error; it.increment(error))
        if (it->is_regular_file(error) && lower(path_utf8(it->path().extension())) == ".json") files.push_back(it->path());
    std::ranges::sort(files);
    for (const auto& file : files) {
        auto name = lower(path_utf8(file.stem()));
        std::ranges::replace(name, '~', '/');
        try {
            const auto bytes = read_file(file);
            out.push_back({mod.name, std::move(name), std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size())});
        } catch (const std::exception& failed) {
            report.notes.push_back(mod.name + ": reskate-ops/" + path_utf8(file.filename()) + " could not be read (" +
                                   failed.what() + ")");
        }
    }
    return out;
}

void replay_asset_ops(AssetOverrides& out, const std::vector<AssetOps>& ops, const std::vector<OpsVersion>& versions,
                      const fs::path& gameRoot, MergeReport& report) {
    // By asset: the mods with ops for it, highest priority first (the order they were read in).
    std::map<std::string, std::vector<const AssetOps*>, std::less<>> byAsset;
    for (const auto& entry : ops) byAsset[entry.name].push_back(&entry);
    for (const auto& [name, mine] : byAsset) {
        std::set<std::string> opsMods;
        for (const auto* entry : mine) opsMods.insert(entry->mod);
        std::set<fb::Sha1> games;
        for (const auto& version : versions)
            if (version.name == name) games.insert(version.game);
        for (const auto& game : games) {
            // The highest mod without ops for the asset: its copy is what the ops are made on.
            const OpsVersion* base{};
            std::set<fb::Guid> names;
            for (const auto& version : versions) {
                if (version.name != name || version.game != game) continue;
                names.insert(version.names.begin(), version.names.end());
                if (!base && !opsMods.contains(version.mod)) base = &version;
            }
            if (!base) continue;   // only mods with ops change it: their own copy stands
            const auto held = out.changed.find(name);
            if (held == out.changed.end() || !held->second.contains(game)) continue;
            std::string failure;
            std::vector<std::byte> bytes;
            std::size_t count{};
            std::string who = base->mod;
            try {
                bytes = fb::decode_cas(base->encoded, {gameRoot});
                // Lowest priority first: a higher mod's edits are made last.
                for (auto at = mine.rbegin(); at != mine.rend() && failure.empty(); ++at) {
                    auto applied = fb::ebx_ops::apply(bytes, (*at)->json);
                    if (!applied.error.empty()) {
                        failure = (*at)->mod + "'s edits could not be made on " + base->mod + "'s copy (" + applied.error + ")";
                        break;
                    }
                    bytes = std::move(applied.bytes);
                    count += applied.lines.size();
                    who += " + " + (*at)->mod;
                }
            } catch (const std::exception& failed) {
                failure = std::string("its copies could not be read (") + failed.what() + ")";
            }
            if (!failure.empty()) {
                report.notes.push_back(name + ": " + failure + "; " + held->second.at(game).mod + "'s copy is kept");
                continue;
            }
            AssetOverride kept{who, sha1_of(bytes), bytes.size(), fb::encode_cas(bytes, {gameRoot}), std::nullopt, std::move(names)};
            kept.reconciled = true;
            held->second.insert_or_assign(game, std::move(kept));
            report.notes.push_back(name + ": " + who + " (the edits of a mod with reskate-ops made on " + base->mod +
                                   "'s copy, " + std::to_string(count) + " step(s))");
        }
    }
}

} // namespace dingosdk::mods::detail
