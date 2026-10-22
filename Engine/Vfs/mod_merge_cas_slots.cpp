#include "mod_merge_internal.h"
#include "lua_script_merge.h"
#include "Engine/Resource/cas_codec.h"
#include "Engine/Resource/ebx_document.h"
#include "Engine/Resource/ebx_writer.h"

#include <algorithm>
#include <map>
#include <set>

// Mods that add skater slots, together (2026-10-19: Kori's Skaterize beside Physical Lanyards' Accessories tab).
// ReSkate combines the slots the mods append to the CAS recipe template, but keeps one mod's copy of everything else
// they change whole: the skater recipes (which list one equip per template slot, IN THE TEMPLATE'S ORDER - a recipe
// shorter than or out of line with the template leaves the skater invisible), the appearance loader's prefab (its
// slot definitions, its Lua script) and the loader script itself. Settled here once, for every bundle and every map's
// copy alike (mod_merge_combine.cpp writes the results wherever the asset is):
//  - the template: the combined one, a slot two copies append (a map's level bundles, each copy rewritten with the
//    same mod's change) listed once;
//  - each skater recipe a mod changes: the highest mod's, its equips put in the combined template's order by their
//    slot hash, an equip for a slot it does not know taken from another mod's version or the game's, else empty;
//  - a mod's own copy of a game Lua script (Skaterize ships the skater loader as a new asset and points the prefab at
//    it): its changes are combined into the game's script with the other mods' (lua_script_merge), and a prefab that
//    points at that copy points at the game's script again, so each mod's changes run;
//  - the prefab: the version whose slot definitions hold every other version's, its Lua script as above.
namespace dingosdk::mods::detail {
namespace {
namespace ebx = fb::ebx;

constexpr std::string_view template_name = "gameplay/characters/data/recipetemplate/casrecipetemplate";
constexpr std::string_view prefab_name = "characters/maincharacters/generic/cas/common/loaders/cas_rsp_lualoader_prefab";
constexpr std::string_view recipe_folder = "gameplay/characters/data/recipe/";
constexpr std::string_view default_recipe = "gameplay/characters/data/recipe/cas_rsp_defaultrecipe";
constexpr std::uint32_t cas_template_key = 2759515148U;   // djb2-xor of "CASRecipeTemplate"

const ebx::Object* object_of(const ebx::Value& value) {
    const auto* held = std::get_if<std::shared_ptr<ebx::Object>>(&value.data);
    return held ? held->get() : nullptr;
}
ebx::Object* object_of(ebx::Value& value) {
    auto* held = std::get_if<std::shared_ptr<ebx::Object>>(&value.data);
    return held ? held->get() : nullptr;
}
const ebx::Value* field(const ebx::Object& object, std::string_view name) {
    for (const auto& entry : object.fields) if (entry.name == name) return &entry.value;
    return nullptr;
}
ebx::Value* field(ebx::Object& object, std::string_view name) {
    for (auto& entry : object.fields) if (entry.name == name) return &entry.value;
    return nullptr;
}
std::optional<std::uint64_t> number(const ebx::Value* value) {
    if (!value) return std::nullopt;
    if (const auto* u = std::get_if<std::uint64_t>(&value->data)) return *u;
    if (const auto* i = std::get_if<std::int64_t>(&value->data)) return static_cast<std::uint64_t>(*i);
    return std::nullopt;
}
const std::string* text(const ebx::Value* value) { return value ? std::get_if<std::string>(&value->data) : nullptr; }
const ebx::Object* root_object(const ebx::Document& document) {
    const auto* root = document.root();
    return root ? root->object.get() : nullptr;
}
ebx::Object* root_object(ebx::Document& document) {
    const auto* root = document.root();
    return root ? root->object.get() : nullptr;
}
// What an external pointer names: the import's file and instance.
std::optional<ebx::ImportReference> import_of(const ebx::Document& document, const ebx::Value* value) {
    const auto* pointer = value ? std::get_if<ebx::PointerReference>(&value->data) : nullptr;
    if (!pointer || pointer->kind != ebx::PointerKind::external || pointer->index < 0 ||
        static_cast<std::size_t>(pointer->index) >= document.imports.size())
        return std::nullopt;
    return document.imports[static_cast<std::size_t>(pointer->index)];
}
bool same_import(const ebx::ImportReference& a, const ebx::ImportReference& b) {
    return a.fileGuid == b.fileGuid && a.classGuid == b.classGuid;
}
using SlotKey = std::pair<fb::Guid, fb::Guid>;   // the slot asset an entry of the template's Slots names

ebx::Value::Array* template_slot_array(ebx::Document& document) {
    auto* root = root_object(document);
    auto* slots = root ? field(*root, "Slots") : nullptr;
    return slots ? std::get_if<ebx::Value::Array>(&slots->data) : nullptr;
}
std::optional<std::vector<SlotKey>> template_slots(const ebx::Document& document) {
    const auto* root = root_object(document);
    const auto* slots = root ? field(*root, "Slots") : nullptr;
    const auto* list = slots ? std::get_if<ebx::Value::Array>(&slots->data) : nullptr;
    if (!list) return std::nullopt;
    std::vector<SlotKey> out;
    for (const auto& entry : *list) {
        const auto* slot = object_of(entry);
        const auto named = slot ? import_of(document, field(*slot, "Slot")) : std::nullopt;
        if (!named) return std::nullopt;
        out.emplace_back(named->fileGuid, named->classGuid);
    }
    return out;
}
bool is_cas_recipe(const ebx::Document& document) {
    const auto* root = root_object(document);
    return root && document.rootType == "DingoAuthoredCharacterRecipeAsset" &&
           (number(field(*root, "TemplateKey")).value_or(0) & 0xFFFFFFFFU) == cas_template_key;
}
ebx::Value::Array* recipe_equips(ebx::Document& document) {
    auto* root = root_object(document);
    auto* compiled = root ? field(*root, "CompiledRecipe") : nullptr;
    auto* recipe = compiled ? object_of(*compiled) : nullptr;
    auto* equips = recipe ? field(*recipe, "Equips") : nullptr;
    return equips ? std::get_if<ebx::Value::Array>(&equips->data) : nullptr;
}
std::optional<std::vector<std::uint32_t>> recipe_slot_hashes(ebx::Document& document) {
    const auto* equips = recipe_equips(document);
    if (!equips) return std::nullopt;
    std::vector<std::uint32_t> out;
    for (const auto& entry : *equips) {
        const auto* equip = object_of(entry);
        const auto hash = equip ? number(field(*equip, "SlotHash")) : std::nullopt;
        if (!hash) return std::nullopt;
        out.push_back(static_cast<std::uint32_t>(*hash));
    }
    return out;
}
// A deep copy (equips hold plain values and one array of floats; nothing they hold is shared).
ebx::Value clone(const ebx::Value& value) {
    ebx::Value out;
    out.arrayHash = value.arrayHash;
    if (const auto* object = object_of(value)) {
        auto copy = std::make_shared<ebx::Object>(*object);
        for (auto& entry : copy->fields) entry.value = clone(entry.value);
        out.data = std::move(copy);
    } else if (const auto* list = std::get_if<ebx::Value::Array>(&value.data)) {
        ebx::Value::Array copy;
        copy.reserve(list->size());
        for (const auto& entry : *list) copy.push_back(clone(entry));
        out.data = std::move(copy);
    } else {
        out.data = value.data;
    }
    return out;
}
// `into` (a copy of one of the kept recipe's own equips) made to hold `from`'s equip: its slot, item and parameters.
void take_equip(ebx::Object& into, const ebx::Object& from) {
    for (const char* name : {"SlotHash", "AssetKeyHash", "AssetKey", "FloatParams"}) {
        auto* to = field(into, name);
        const auto* value = field(from, name);
        if (to && value) {
            const auto hash = to->arrayHash;
            *to = clone(*value);
            if (std::holds_alternative<ebx::Value::Array>(to->data)) to->arrayHash = hash ? hash : value->arrayHash;
        }
    }
}
void blank_equip(ebx::Object& equip, std::uint32_t slot) {
    if (auto* hash = field(equip, "SlotHash")) {
        if (std::holds_alternative<std::int64_t>(hash->data)) hash->data = static_cast<std::int64_t>(slot);
        else hash->data = static_cast<std::uint64_t>(slot);
    }
    if (auto* key = field(equip, "AssetKeyHash")) {
        if (std::holds_alternative<std::int64_t>(key->data)) key->data = std::int64_t{0};
        else key->data = std::uint64_t{0};
    }
    if (auto* key = field(equip, "AssetKey")) key->data = std::string{};
    if (auto* params = field(equip, "FloatParams"))
        if (auto* list = std::get_if<ebx::Value::Array>(&params->data)) list->clear();
}
const ebx::Object* equip_for(ebx::Document& document, std::uint32_t slot) {
    auto* equips = recipe_equips(document);
    if (!equips) return nullptr;
    for (const auto& entry : *equips)
        if (const auto* equip = object_of(entry); equip && number(field(*equip, "SlotHash")) == slot) return equip;
    return nullptr;
}
// The loader prefab's DingoLuaAppearanceLoaderEntityData.
ebx::Object* loader_data(ebx::Document& document) {
    for (auto& instance : document.instances)
        if (instance.object && instance.object->descriptor >= 0 &&
            static_cast<std::size_t>(instance.object->descriptor) < document.types.size() &&
            document.types[static_cast<std::size_t>(instance.object->descriptor)].name == "DingoLuaAppearanceLoaderEntityData")
            return instance.object.get();
    return nullptr;
}
// The names (LuaVarName) of a loader prefab's slot definitions.
std::set<std::string> slot_definitions(ebx::Document& document) {
    std::set<std::string> out;
    auto* data = loader_data(document);
    auto* definitions = data ? field(*data, "SlotDefinitions") : nullptr;
    const auto* list = definitions ? std::get_if<ebx::Value::Array>(&definitions->data) : nullptr;
    if (!list) return out;
    for (const auto& entry : *list) {
        const auto* pointer = std::get_if<ebx::PointerReference>(&entry.data);
        if (!pointer || pointer->kind != ebx::PointerKind::internal || pointer->index < 0 ||
            static_cast<std::size_t>(pointer->index) >= document.instances.size())
            continue;
        const auto& target = document.instances[static_cast<std::size_t>(pointer->index)];
        if (target.object)
            if (const auto* name = text(field(*target.object, "LuaVarName"))) out.insert(*name);
    }
    return out;
}

struct Decoded {
    ebx::Document document;
    bool ok{};
};
Decoded decode(std::span<const std::byte> encoded, const fs::path& gameRoot) {
    Decoded out;
    try {
        out.document = ebx::read_document(fb::decode_cas(encoded, {gameRoot}));
        out.ok = true;
    } catch (const std::exception&) {}
    return out;
}
void store_kept(AssetOverride& kept, const ebx::Document& document, const fs::path& gameRoot) {
    const auto bytes = ebx::write_document(document);
    kept.sha1 = sha1_of(bytes);
    kept.originalSize = bytes.size();
    kept.encoded = fb::encode_cas(bytes, {gameRoot});
    kept.reconciled = true;
}
} // namespace

void reconcile_cas_slots(AssetOverrides& out, const std::vector<CasSlotVersion>& versions,
                         const std::map<std::pair<std::string, fb::Sha1>, std::vector<std::byte>>& gameCopies,
                         const std::map<std::string, std::vector<std::byte>, std::less<>>& gameScripts,
                         const fs::path& gameRoot, MergeReport& report) {
    // -- A mod's own copy of a game Lua script, combined into the game's script ------------------------------------
    std::set<fb::Guid> mergedScriptAssets;   // the LuaAsset EBX of the copies whose changes now are in the game's
    for (auto& [scriptName, byGame] : out.scripts) {
        const auto gameEncoded = gameScripts.find(scriptName);
        if (gameEncoded == gameScripts.end()) continue;
        std::vector<std::byte> game;
        std::optional<LuaScript> gameScript;
        try {
            game = fb::decode_cas(gameEncoded->second, {gameRoot});
            gameScript = read_lua_script(game);
        } catch (const std::exception&) {}
        if (!gameScript) continue;
        std::set<std::string> done;
        for (auto& [gameSha, kept] : byGame) {
            if (!kept.resource) continue;
            for (const auto& [bundle, additions] : out.added)
                for (const auto& addition : additions) {
                    if (addition.asset.kind != fb::AssetKind::resource || addition.asset.resourceType != luaScriptResourceType)
                        continue;
                    const auto copyName = lower(addition.asset.name);
                    if (copyName == scriptName || done.contains(copyName)) continue;
                    std::vector<std::byte> copy;
                    try { copy = fb::decode_cas(addition.encoded, {gameRoot}); } catch (const std::exception&) { continue; }
                    const auto copyScript = read_lua_script(copy);
                    if (!copyScript || copyScript->name != gameScript->name) continue;
                    done.insert(copyName);
                    std::vector<std::byte> mine;
                    try { mine = fb::decode_cas(kept.encoded, {gameRoot}); } catch (const std::exception&) { continue; }
                    // The copy's mod is the lower one here: where both changed the same lines, the version in force wins.
                    const auto merged = merge_lua_script(game, mine, copy);
                    if (!merged) {
                        report.notes.push_back(addition.mod + ": its copy of " + gameScript->name + " (" + copyName +
                            ") changes the same lines as " + kept.mod + "'s " + scriptName +
                            " differently; the two cannot run together");
                        continue;
                    }
                    kept.sha1 = sha1_of(*merged);
                    kept.originalSize = merged->size();
                    kept.encoded = fb::encode_cas(*merged, {gameRoot});
                    const auto meta = lua_script_meta(*merged);
                    kept.resource->resourceMeta.assign(meta.begin(), meta.end());
                    kept.resource->originalSize = merged->size();
                    kept.resource->sha1 = kept.sha1;
                    if (!kept.mod.ends_with(" + " + addition.mod)) kept.mod += " + " + addition.mod;
                    for (const auto& [otherBundle, list] : out.added)
                        for (const auto& wrapper : list)
                            if (wrapper.asset.kind == fb::AssetKind::ebx && lower(wrapper.asset.name) == copyName)
                                mergedScriptAssets.insert(wrapper.file);
                    report.notes.push_back(addition.mod + "'s own copy of " + gameScript->name + " (" + copyName +
                        ") combined into " + scriptName + " with the other mods' changes; its prefab runs that one");
                }
        }
    }

    // -- The template --------------------------------------------------------------------------------------------------
    std::optional<std::vector<SlotKey>> slots;
    std::size_t addedSlots{};
    if (const auto found = out.changed.find(template_name); found != out.changed.end())
        for (auto& [gameSha, kept] : found->second) {
            auto decoded = decode(kept.encoded, gameRoot);
            if (!decoded.ok) continue;
            auto* list = template_slot_array(decoded.document);
            const auto listed = template_slots(decoded.document);
            if (!list || !listed) continue;
            std::set<SlotKey> seen;
            ebx::Value::Array unique;
            for (std::size_t i = 0; i < list->size(); ++i)
                if (seen.insert((*listed)[i]).second) unique.push_back(std::move((*list)[i]));
            const auto repeated = list->size() - unique.size();
            *list = std::move(unique);
            const bool several = kept.mod.find(" + ") != std::string::npos;
            if (repeated || several) {
                store_kept(kept, decoded.document, gameRoot);
                if (repeated)
                    report.notes.push_back(std::string(template_name) + ": " + std::to_string(repeated) +
                        " slot(s) more than one copy appended listed once");
            }
            slots = template_slots(decoded.document);
            if (const auto game = gameCopies.find({std::string(template_name), gameSha}); game != gameCopies.end()) {
                auto base = decode(game->second, gameRoot);
                if (base.ok)
                    if (const auto gameSlots = template_slots(base.document); gameSlots && slots && slots->size() > gameSlots->size())
                        addedSlots = slots->size() - gameSlots->size();
            }
            break;   // one template: the CAS one
        }

    // -- Which slot each template entry is: a template and a recipe of the same copy line up ---------------------------
    std::map<SlotKey, std::uint32_t> hashOf;
    const auto learn = [&](ebx::Document& templateDoc, ebx::Document& recipeDoc) {
        const auto keys = template_slots(templateDoc);
        const auto hashes = recipe_slot_hashes(recipeDoc);
        if (!keys || !hashes || keys->size() != hashes->size()) return;
        for (std::size_t i = 0; i < keys->size(); ++i) hashOf.emplace((*keys)[i], (*hashes)[i]);
    };
    {
        std::optional<Decoded> gameTemplate, gameRecipe;
        for (const auto& [key, bytes] : gameCopies) {
            if (key.first == template_name && !gameTemplate) gameTemplate = decode(bytes, gameRoot);
            if (key.first == default_recipe && !gameRecipe) gameRecipe = decode(bytes, gameRoot);
        }
        if (gameTemplate && gameRecipe && gameTemplate->ok && gameRecipe->ok) learn(gameTemplate->document, gameRecipe->document);
        std::map<std::string, std::vector<const CasSlotVersion*>> byMod;
        for (const auto& version : versions) byMod[version.mod].push_back(&version);
        for (const auto& [mod, own] : byMod) {
            const CasSlotVersion* templateVersion{};
            std::vector<const CasSlotVersion*> recipes;
            for (const auto* version : own) {
                if (version->name == template_name) templateVersion = version;
                else if (version->name.starts_with(recipe_folder)) recipes.push_back(version);
            }
            if (!templateVersion) continue;
            auto templateDoc = decode(templateVersion->encoded, gameRoot);
            if (!templateDoc.ok) continue;
            const auto keys = template_slots(templateDoc.document);
            // its default recipe first, else any of its skater recipes as long as the template
            std::stable_sort(recipes.begin(), recipes.end(), [](const auto* a, const auto* b) {
                return (a->name == default_recipe) > (b->name == default_recipe); });
            for (const auto* recipe : recipes) {
                auto recipeDoc = decode(recipe->encoded, gameRoot);
                if (!recipeDoc.ok || !is_cas_recipe(recipeDoc.document)) continue;
                const auto hashes = recipe_slot_hashes(recipeDoc.document);
                if (!keys || !hashes || hashes->size() != keys->size()) continue;
                learn(templateDoc.document, recipeDoc.document);
                break;
            }
        }
    }

    // -- The skater recipes --------------------------------------------------------------------------------------------
    if (slots) {
        std::vector<std::uint32_t> order;
        bool known = true;
        for (const auto& key : *slots) {
            const auto hash = hashOf.find(key);
            if (hash == hashOf.end()) { known = false; break; }
            order.push_back(hash->second);
        }
        if (!known) {
            report.notes.push_back(std::string(template_name) + ": a mod adds a slot no recipe of it names; the skater "
                "recipes are left as the mods made them");
        } else {
            std::size_t rebuilt{}, filled{};
            for (auto& [name, byGame] : out.changed) {
                if (!name.starts_with(recipe_folder)) continue;
                for (auto& [gameSha, kept] : byGame) {
                    auto decoded = decode(kept.encoded, gameRoot);
                    if (!decoded.ok || !is_cas_recipe(decoded.document)) continue;
                    const auto current = recipe_slot_hashes(decoded.document);
                    if (!current || *current == order) continue;
                    auto* equips = recipe_equips(decoded.document);
                    if (!equips || equips->empty()) continue;
                    // The other versions of this recipe (priority order) and the game's, for equips it does not have.
                    std::vector<Decoded> others;
                    for (const auto& version : versions)
                        if (version.name == name && version.game == gameSha) others.push_back(decode(version.encoded, gameRoot));
                    if (const auto game = gameCopies.find({name, gameSha}); game != gameCopies.end())
                        others.push_back(decode(game->second, gameRoot));
                    // A copy of one of its own empty equips is the shape every new one takes.
                    const ebx::Value* shape = &equips->front();
                    for (const auto& entry : *equips)
                        if (const auto* equip = object_of(entry); equip && text(field(*equip, "AssetKey")) &&
                            text(field(*equip, "AssetKey"))->empty()) { shape = &entry; break; }
                    ebx::Value::Array next;
                    next.reserve(order.size());
                    for (const auto slot : order) {
                        if (const auto* own = equip_for(decoded.document, slot)) {
                            ebx::Value copy;
                            for (const auto& entry : *equips)
                                if (object_of(entry) == own) { copy = clone(entry); break; }
                            next.push_back(std::move(copy));
                            continue;
                        }
                        auto made = clone(*shape);
                        auto* equip = object_of(made);
                        blank_equip(*equip, slot);
                        for (auto& other : others)
                            if (other.ok)
                                if (const auto* theirs = equip_for(other.document, slot)) { take_equip(*equip, *theirs); break; }
                        next.push_back(std::move(made));
                        ++filled;
                    }
                    *equips = std::move(next);
                    store_kept(kept, decoded.document, gameRoot);
                    ++rebuilt;
                }
            }
            if (rebuilt)
                report.notes.push_back(std::to_string(rebuilt) + " skater recipe(s) follow the combined template's " +
                    std::to_string(order.size()) + " slots (" + std::to_string(addedSlots) + " added by mods; " +
                    std::to_string(filled) + " equip(s) filled in)");
        }
    }

    // -- The loader prefab ---------------------------------------------------------------------------------------------
    if (const auto found = out.changed.find(prefab_name); found != out.changed.end())
        for (auto& [gameSha, kept] : found->second) {
            auto keptDoc = decode(kept.encoded, gameRoot);
            if (!keptDoc.ok) continue;
            std::vector<Decoded> candidates;
            candidates.push_back(std::move(keptDoc));
            for (const auto& version : versions)
                if (version.name == prefab_name && version.game == gameSha) candidates.push_back(decode(version.encoded, gameRoot));
            std::vector<std::set<std::string>> names;
            std::set<std::string> all;
            for (auto& candidate : candidates) {
                names.push_back(candidate.ok ? slot_definitions(candidate.document) : std::set<std::string>{});
                all.insert(names.back().begin(), names.back().end());
            }
            // The version that already holds every other version's slot definitions (instances cannot be carried
            // over here); none: the one in force, and the slots it lacks are named.
            std::size_t pick = 0;
            for (std::size_t i = 0; i < candidates.size(); ++i)
                if (candidates[i].ok && names[i] == all) { pick = i; break; }
            bool changed = pick != 0;
            auto& chosen = candidates[pick].document;
            if (names[pick] != all) {
                std::string missing;
                for (const auto& name : all) if (!names[pick].contains(name)) missing += (missing.empty() ? "" : ", ") + name;
                report.notes.push_back(std::string(prefab_name) + ": slot definition(s) " + missing +
                    " of another mod could not be added to the one in force");
            }
            // A Lua script that is a copy now combined into the game's: the game's again.
            if (auto* data = loader_data(chosen)) {
                auto* lua = field(*data, "LuaAsset");
                const auto named = import_of(chosen, lua);
                if (named && mergedScriptAssets.contains(named->fileGuid)) {
                    const auto game = gameCopies.find({std::string(prefab_name), gameSha});
                    auto base = game != gameCopies.end() ? decode(game->second, gameRoot) : Decoded{};
                    auto* baseData = base.ok ? loader_data(base.document) : nullptr;
                    const auto gameLua = baseData ? import_of(base.document, field(*baseData, "LuaAsset")) : std::nullopt;
                    if (gameLua) {
                        auto at = std::find_if(chosen.imports.begin(), chosen.imports.end(),
                                               [&](const ebx::ImportReference& reference) { return same_import(reference, *gameLua); });
                        if (at == chosen.imports.end()) at = chosen.imports.insert(chosen.imports.end(), *gameLua);
                        lua->data = ebx::PointerReference{ebx::PointerKind::external,
                                                           static_cast<std::int32_t>(at - chosen.imports.begin())};
                        changed = true;
                        report.notes.push_back(std::string(prefab_name) + ": runs the game's loader script again, which "
                            "now holds the changes the mods' own copy of it made");
                    }
                }
            }
            // Several mods' versions: the one chosen goes into every copy (a bundle's own copies would otherwise be
            // settled there again, the highest-priority copy kept whole - a costume mod's copy without the slots).
            std::set<std::string> mods;
            for (const auto& version : versions)
                if (version.name == prefab_name && version.game == gameSha) mods.insert(version.mod);
            if (changed) {
                store_kept(kept, chosen, gameRoot);
                if (pick) kept.mod += " (with " + std::to_string(all.size()) + " slot definitions)";
            } else if (mods.size() > 1 || kept.mod.find(" + ") != std::string::npos) {
                kept.reconciled = true;
            }
        }
}

} // namespace dingosdk::mods::detail
