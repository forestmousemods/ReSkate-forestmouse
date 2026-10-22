#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Which tiles of an item grid are shown, and in what order. Pure: no game state, so it is tested on its own.
namespace dingosdk::item_browser {
enum class Filter : std::uint8_t { all, favorites, mods, game };
inline constexpr std::string_view filter_names[]{"All", "Favorites", "Mods", "Game items"};

// The kinds of item the type tabs over a grid offer, in the order they are shown. A grid only ever
// offers the kinds of its own slot: tops, bottoms, shoes and so on.
inline constexpr std::string_view kind_names[]{
    "T-Shirts", "Longsleeves", "Hoodies", "Sweatshirts", "Shirts", "Tanks", "Crop Tops", "Jackets", "Dresses",
    "Jeans", "Cargo", "Chinos", "Track Pants", "Leggings", "Pants", "Shorts", "Skirts",
    "Low Tops", "High Tops", "Slip-Ons", "Boots", "Flats",
    "Caps", "Beanies", "Bucket Hats", "Hats", "Helmets", "Masks",
    "Sunglasses", "Glasses",
    "Low Socks", "Mid Calf", "Knee High", "Thigh High",
    "Overalls", "Onesies", "Swimwear", "Robes",
    "Lanyards", "Keychains", "Chains",
    "Costumes", "Other"};

// The slot a grid shows, from the asset names of the game's items in it ("own_topshirt_...").
enum class Slot : std::uint8_t { unknown, tops, bottoms, shoes, headwear, eyewear, socks, outfits, costumes };

struct Item {
    std::string key, title, category;
    bool modded{};
    std::uint64_t content{}; // the tile's native item model
    // What the item is fitted on (its data's first morph preset or mesh, "Gen_Shirt_HoodieRelaxed_complex_dmPreset"):
    // a mod's item made on one of the game's shapes is of that shape's kind.
    std::string shape;
    std::string_view kind;   // one of kind_names (assign_kinds), empty for a tile that shows no item
};

inline std::string folded(std::string_view text) {
    std::string result(text);
    for (auto& c : result) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return result;
}

// Our accessories (Physical_Lanyards) come as one item per place. fit18: one tile per model. A key's model is the key
// without its place (lower case), its place one of its kind's place words. The grids show each model once, the item of
// its kind's first place (keychains and lanyards: the left back one, whose key names no place; wallet chains: the left
// front one); the item panel picks the place, and the skater wears the model's item there.
enum class AccessoryKind : std::uint8_t { keychain, lanyard, wallet };
inline constexpr std::string_view keychain_places[]{"", "_front", "_side", "_right", "_front_right", "_side_right",
                                                    "_front_clip", "_front_clip_right"};
inline constexpr std::string_view lanyard_places[]{"", "_front", "_side", "_right", "_front_right", "_side_right"};
inline constexpr std::string_view wallet_places[]{"_front", "_back", "_front_right", "_back_right"};
struct AccessoryVariant {
    AccessoryKind kind{};
    std::string model, place;
    bool shown{}; // the model's tile
    // fit28: what hangs on the carabiner (a keychain's charm set, "" = the model's own) and the item without our sounds
    std::string charm;
    bool quiet{};
};
// fit28 (user 2026-10-16 / 10-19: keys / key + fingerboard + dice / key + dice + 8-ball): a keychain model's charm
// sets come as items of their own whose key adds the set before the place, a quiet twin adds "_quiet" after it:
// <model><set><_quiet><place>, e.g. own_keychain_angular_navy_keys_quiet_front_clip.
inline constexpr std::string_view charm_sets[]{"", "_keys", "_board", "_ball"};
inline constexpr std::string_view quiet_word = "_quiet";
inline std::string_view charm_title(std::string_view set) {
    if (set == "_keys") return "Keys";
    if (set == "_board") return "Key + board + dice";
    if (set == "_ball") return "Key + dice + 8-ball";
    return "Original set";
}
// KC39: a model's own set by what it is (the camo carabiner came with keys, Cross Black with the 8-ball, Angular Navy
// with the board); the others' own charms are their own.
inline std::string_view charm_title(std::string_view set, std::string_view model) {
    if (set.empty()) {
        if (model == "own_keychain_carabiner_camo") return charm_title("_keys");
        if (model == "own_keychain_cross_black") return charm_title("_ball");
        if (model == "own_keychain_angular_navy") return charm_title("_board");
    }
    return charm_title(set);
}
inline std::string variant_key(std::string_view model, std::string_view charm, bool quiet, std::string_view place) {
    return std::string(model) + std::string(charm) + (quiet ? std::string(quiet_word) : std::string()) + std::string(place);
}
inline std::span<const std::string_view> accessory_places(AccessoryKind kind) {
    if (kind == AccessoryKind::keychain) return keychain_places;
    if (kind == AccessoryKind::lanyard) return lanyard_places;
    return wallet_places;
}
inline std::optional<AccessoryVariant> accessory_variant(std::string_view key) {
    const auto k = folded(key);
    constexpr std::pair<std::string_view, AccessoryKind> families[]{
        {"own_keychain_", AccessoryKind::keychain}, {"own_lanyard_", AccessoryKind::lanyard},
        {"own_wallet_chain_", AccessoryKind::wallet}};
    for (const auto& [prefix, kind] : families) {
        if (!k.starts_with(prefix)) continue;
        const auto places = accessory_places(kind);
        std::string_view place;
        bool found = places.front().empty(); // keychains / lanyards: no place word is the first place
        for (const auto p : places)          // the longest place word that leaves a model name
            if (!p.empty() && k.size() > prefix.size() + p.size() && k.ends_with(p) && p.size() >= place.size())
                place = p, found = true;
        if (!found) return std::nullopt;
        AccessoryVariant v{kind, k.substr(0, k.size() - place.size()), std::string(place), place == places.front()};
        if (v.model.size() > prefix.size() + quiet_word.size() && v.model.ends_with(quiet_word))
            v.quiet = true, v.model.resize(v.model.size() - quiet_word.size());
        if (kind == AccessoryKind::keychain)
            for (const auto set : charm_sets)
                if (!set.empty() && v.model.size() > prefix.size() + set.size() && v.model.ends_with(set)) {
                    v.charm = std::string(set), v.model.resize(v.model.size() - set.size());
                    break;
                }
        return v;
    }
    return std::nullopt;
}
// The place a model's tile shows before the player picks one (user 2026-10-19: the keychains by their clip at the
// front left, no strap; lanyards at the back left; wallet chains at the front left).
inline std::string_view default_place(AccessoryKind kind) {
    return kind == AccessoryKind::keychain ? std::string_view("_front_clip") : accessory_places(kind).front();
}
// The places picked (model -> place word), from the accessory panel. fit28: also a model's charm set
// (charm_pick(model) -> set) and the sound for all of them (sound_pick -> "off" for the quiet twins).
using AccessoryPicks = std::map<std::string, std::string, std::less<>>;
inline std::string charm_pick(std::string_view model) { return std::string(model) + "#charm"; }
inline constexpr std::string_view sound_pick = "#sound";
inline std::string_view picked_charm(const AccessoryVariant& v, const AccessoryPicks* picks) {
    if (picks)
        if (const auto found = picks->find(charm_pick(v.model)); found != picks->end()) return found->second;
    return {};
}
inline bool picked_quiet(const AccessoryPicks* picks) {
    if (!picks) return false;
    const auto found = picks->find(sound_pick);
    return found != picks->end() && found->second == "off";
}
inline std::string_view picked_place(const AccessoryVariant& v, const AccessoryPicks* picks) {
    if (picks)
        if (const auto found = picks->find(v.model); found != picks->end()) return found->second;
    return default_place(v.kind);
}
// The row a native row stands for in a grid with one tile per accessory model: itself for anything else; for an
// accessory, its model's picked place row when it is the model's first row, else none (the model is already shown).
// `first` and `picked` are filled once per arrangement (accessory_rows).
struct AccessoryRows {
    std::map<std::string, unsigned, std::less<>> first, picked;
};
inline AccessoryRows accessory_rows(std::span<const struct Item> native, const AccessoryPicks* picks);
// A keychain hangs from the strap at every place, or only by its clip, at the front (left or right).
inline bool clip_place(std::string_view place) { return place.starts_with("_front_clip"); }
inline std::string_view strap_toggled(std::string_view place) {
    const bool right = place.ends_with("_right");
    if (clip_place(place)) return right ? "_front_right" : "_front";
    return right ? "_front_clip_right" : "_front_clip";
}
// The places the panel's place row goes through: the kind's, for a keychain those of its group (strap or clip).
inline std::vector<std::string_view> place_group(AccessoryKind kind, std::string_view place) {
    std::vector<std::string_view> out;
    for (const auto p : accessory_places(kind))
        if (kind != AccessoryKind::keychain || clip_place(p) == clip_place(place)) out.push_back(p);
    return out;
}
inline std::string_view place_title(std::string_view place) {
    if (place.empty() || place == "_back") return "Back";
    if (place == "_front" || place == "_front_clip") return "Front";
    if (place == "_side") return "Side";
    if (place == "_right" || place == "_back_right") return "Back right";
    if (place == "_front_right" || place == "_front_clip_right") return "Front right";
    if (place == "_side_right") return "Side right";
    return "Other";
}

namespace kinds_detail {
using Needles = std::initializer_list<std::string_view>;

// The slot an asset name's first word belongs to: "topshirt", "bottompants", "shoesneaker".
inline Slot slot_of(std::string_view family) {
    if (family == "costume" || family.ends_with("costume")) return Slot::costumes;
    if (family.starts_with("top")) return Slot::tops;
    if (family.starts_with("bottom")) return Slot::bottoms;
    if (family.starts_with("shoe")) return Slot::shoes;
    if (family.starts_with("headwear")) return Slot::headwear;
    if (family.starts_with("eyewear")) return Slot::eyewear;
    if (family.starts_with("sock")) return Slot::socks;
    if (family.starts_with("outfit")) return Slot::outfits;
    // Our accessories (Physical Lanyards), in the outfits grid or in their own Accessories slot.
    if (family == "keychain" || family == "lanyard" || family == "wallet" || family == "carabiner") return Slot::outfits;
    return Slot::unknown;
}
inline std::string family_of(const std::string& key) { // key folded, "own_" taken off
    return key.substr(0, key.find('_'));
}

// Asset names run their words together ("hoodierelaxed"): any needle inside them counts.
struct Joined {
    std::string text;
    bool operator()(Needles needles) const {
        return std::any_of(needles.begin(), needles.end(), [&](std::string_view n) { return text.find(n) != std::string::npos; });
    }
};
// Titles and mods' names are words: a needle is a whole word, its plural, or two words written apart
// ("Long Sleeve", "T-Shirt"), so "Pantera" is no pants and "Chateau" no hat. A needle of five letters
// or more may also start a word that runs on ("HoodieZip", "Thighhighs"); a number after a word
// ("Beanie8") does not count.
struct Words {
    std::vector<std::string> list;
    explicit Words(std::string_view text) {
        std::string word;
        const auto add = [&] {
            if (word.empty()) return;
            auto bare = word;
            while (!bare.empty() && bare.back() >= '0' && bare.back() <= '9') bare.pop_back();
            if (!bare.empty() && bare != word) list.push_back(bare);
            list.push_back(std::exchange(word, {}));
        };
        for (const char c : text) {
            const char l = c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
            if ((l >= 'a' && l <= 'z') || (l >= '0' && l <= '9')) word += l;
            else add();
        }
        add();
    }
    static bool same(std::string_view word, std::string_view needle) {
        return word == needle || (word.size() == needle.size() + 1 && word.starts_with(needle) && word.back() == 's') ||
               (word.size() == needle.size() + 2 && word.starts_with(needle) && word.ends_with("es")) ||
               (needle.size() >= 5 && word.starts_with(needle));
    }
    bool operator()(Needles needles) const {
        for (const auto needle : needles)
            for (std::size_t i = 0; i < list.size(); ++i)
                if (same(list[i], needle) || (i + 1 < list.size() && same(list[i] + list[i + 1], needle))) return true;
        return false;
    }
};

// The rules of each slot, for asset names and for words alike; empty when nothing says.
template<class Has> std::string_view top(const Has& has) {
    if (has({"costume"})) return "Costumes";
    if (has({"hoodie", "hoody"})) return "Hoodies";
    if (has({"longsleeve", "lgsleeve", "raglan"})) return "Longsleeves";
    if (has({"sleeveless", "tank", "tanktop", "singlet"})) return "Tanks";
    if (has({"crop", "croptop", "cami", "bra", "bralette", "tube"})) return "Crop Tops";
    if (has({"sweatshirt", "crewneck", "crew", "sweater", "fleece", "quarterzip", "knit", "cardigan", "jumper"})) return "Sweatshirts";
    if (has({"buttonup", "button", "buttondown", "butn", "flannel", "polo", "jersey", "camp", "collarless", "overshirt"}))
        return "Shirts";
    if (has({"tshirt", "tee", "ringer", "shortsleeve"})) return "T-Shirts";
    if (has({"jacket", "coat", "puffer", "puffa", "nuptse", "parka", "windbreaker", "anorak", "bomber", "varsity", "letterman", "vest",
             "gilet", "blazer", "smock", "suit"})) return "Jackets";
    if (has({"dress"})) return "Dresses";
    if (has({"shirt"})) return "T-Shirts";
    return {};
}
template<class Has> std::string_view bottom(const Has& has) {
    if (has({"costume"})) return "Costumes";
    if (has({"short", "jort"})) return "Shorts";
    if (has({"skirt"})) return "Skirts";
    if (has({"legging"})) return "Leggings";
    if (has({"track", "trackpant", "jogger", "sweatpant", "sweats"})) return "Track Pants";
    if (has({"cargo"})) return "Cargo";
    if (has({"jean", "denim", "carpenter"})) return "Jeans";
    if (has({"chino", "874", "work", "doubleknee"})) return "Chinos";
    if (has({"pant", "trouser", "slacks"})) return "Pants";
    return {};
}
template<class Has> std::string_view shoe(const Has& has) {
    if (has({"costume"})) return "Costumes";
    if (has({"slipon"})) return "Slip-Ons";
    if (has({"boot"})) return "Boots";
    if (has({"clog", "slipper", "sandal", "slide", "derby", "loafer", "flat"})) return "Flats";
    if (has({"hightop", "high", "mid", "halfcab"})) return "High Tops";
    if (has({"low", "lowtop", "oldskool", "skool", "af1", "airforce", "authentic", "era", "samba", "superstar", "campus",
             "busenitz", "tn", "airmax", "puffy", "puffa", "gat", "gats"})) return "Low Tops";
    return {};
}
template<class Has> std::string_view headwear(const Has& has) {
    if (has({"costume"})) return "Costumes";
    if (has({"helmet", "hardhat"})) return "Helmets";
    if (has({"mask", "balaclava", "goalie"})) return "Masks";
    if (has({"beanie"})) return "Beanies";
    if (has({"bucket"})) return "Bucket Hats";
    if (has({"cap", "trucker", "truck", "snapback", "fitted", "ballcap"})) return "Caps";
    if (has({"hat", "durag", "bandana", "headband", "cowboy"})) return "Hats";
    return {};
}
template<class Has> std::string_view eyewear(const Has& has) {
    if (has({"sunglass", "sunglasses", "shades", "aviator", "visor", "wraparound", "shield", "pilot", "bugeye", "slitted",
             "goggle", "sunnies"})) return "Sunglasses";
    if (has({"glasses", "eyeglass", "eyeglasses", "specs", "rim", "acetate"})) return "Glasses";
    return {};
}
template<class Has> std::string_view socks(const Has& has) {
    if (has({"thigh"})) return "Thigh High";
    if (has({"knee", "kneehigh"})) return "Knee High";
    if (has({"midcalf", "calf", "crew"})) return "Mid Calf";
    if (has({"ankle", "quarter", "low", "noshow"})) return "Low Socks";
    return {};
}
template<class Has> std::string_view outfits(const Has& has) {
    if (has({"lanyard"})) return "Lanyards";
    if (has({"keychain", "carabiner"})) return "Keychains";
    if (has({"wallet", "chain"})) return "Chains";
    if (has({"overall", "overalls", "boilersuit", "jumpsuit", "coverall"})) return "Overalls";
    if (has({"dress"})) return "Dresses";
    if (has({"onesie", "longjohn", "longjohns"})) return "Onesies";
    if (has({"swim", "swimsuit"})) return "Swimwear";
    if (has({"robe", "tunic"})) return "Robes";
    if (has({"costume"})) return "Costumes";
    return {};
}
template<class Has> std::string_view rules(Slot slot, const Has& has) {
    switch (slot) {
    case Slot::tops: return top(has);
    case Slot::bottoms: return bottom(has);
    case Slot::shoes: return shoe(has);
    case Slot::headwear: return headwear(has);
    case Slot::eyewear: return eyewear(has);
    case Slot::socks: return socks(has);
    case Slot::outfits: return outfits(has);
    case Slot::costumes: return "Costumes";
    default: return {};
    }
}

// From an asset name of the slot's own kind: its first word (the game's family, "topjacket", or a
// shape's, "jacket") and the cut after it ("hoodierelaxed"). What a family alone settles comes first.
inline std::string_view by_name(Slot slot, std::string_view family, std::string_view cut) {
    const Joined in_cut{std::string(cut)};
    const auto is = [&](Needles needles) { return Joined{std::string(family)}(needles); };
    switch (slot) {
    case Slot::tops:
        if (is({"costume"})) return "Costumes";
        if (is({"dress"})) return "Dresses";
        if (is({"bra"})) return "Crop Tops";
        if (in_cut({"hoodie"})) return "Hoodies";
        if (is({"jacket", "coat", "vest"})) return "Jackets";
        if (is({"sweater"})) return "Sweatshirts";
        return top(in_cut);
    case Slot::bottoms:
        if (is({"costume"})) return "Costumes";
        if (is({"short"})) return "Shorts";
        if (is({"skirt"})) return "Skirts";
        if (const auto kind = bottom(in_cut); !kind.empty()) return kind;
        return "Pants";
    case Slot::shoes:
        if (is({"costume"})) return "Costumes";
        if (is({"flat"})) return "Flats";
        if (const auto kind = shoe(in_cut); !kind.empty()) return kind;
        // "Sk8-Hi": the cut's last letters.
        if (cut.substr(0, cut.find('_')).ends_with("hi")) return "High Tops";
        return "Low Tops";
    case Slot::headwear:
        if (is({"costume"})) return "Costumes";
        if (is({"helmet"})) return "Helmets";
        if (is({"mask"})) return "Masks";
        if (const auto kind = headwear(in_cut); !kind.empty()) return kind;
        return "Hats";
    case Slot::eyewear:
        if (const auto kind = eyewear(in_cut); !kind.empty()) return kind;
        return "Glasses";
    case Slot::outfits:
        if (const auto kind = outfits(Joined{std::string(family) + '_' + std::string(cut)}); !kind.empty()) return kind;
        return {};
    default: return rules(slot, in_cut);
    }
}
// A shape's words: "gen_shirt_hoodierelaxed_complex_dmpreset" -> family "shirt", cut "hoodierelaxed".
// A mesh a mod made itself is named after a GUID and says nothing.
inline bool shape_words(std::string_view shape, std::string& family, std::string& cut) {
    auto text = folded(shape);
    for (const std::string_view tail : {"_complex_dmpreset", "_dmpreset", "_mesh", "_base"})
        if (text.ends_with(tail)) text.resize(text.size() - tail.size());
    std::vector<std::string> parts;
    for (std::size_t at = 0; at <= text.size();) {
        const auto end = std::min(text.find('_', at), text.size());
        parts.push_back(text.substr(at, end - at));
        at = end + 1;
    }
    const auto plain = [](const std::string& word) {
        return !word.empty() && std::all_of(word.begin(), word.end(), [](char c) { return c >= 'a' && c <= 'z'; });
    };
    if (parts.size() < 3 || !plain(parts[0]) || !plain(parts[1]) || parts[0] == "copy") return false;
    family = parts[1];
    cut.clear();
    for (std::size_t i = 2; i < parts.size(); ++i) cut += (i > 2 ? "_" : "") + parts[i];
    return !cut.empty();
}
} // namespace kinds_detail

// The slot a grid shows: the one most of its items' asset names belong to.
inline Slot grid_slot(std::span<const Item> items) {
    std::array<unsigned, 9> votes{};
    for (const auto& item : items) {
        if (item.key.empty()) continue;
        auto key = folded(item.key);
        if (key.starts_with("own_")) key.erase(0, 4);
        ++votes[static_cast<std::size_t>(kinds_detail::slot_of(kinds_detail::family_of(key)))];
    }
    votes[0] = 0;
    const auto best = std::max_element(votes.begin(), votes.end());
    return *best ? static_cast<Slot>(best - votes.begin()) : Slot::unknown;
}

// What kind an item is among its slot's kinds: by its asset name when that is the game's kind of
// name, else by the game's shape it was made on, else by the words of its title and its name; when
// nothing says, "Other". Empty for a tile that shows no item.
inline std::string_view kind_in(Slot slot, const Item& item) {
    using namespace kinds_detail;
    if (item.key.empty()) return {};
    if (slot == Slot::unknown) return "Other";
    if (slot == Slot::costumes) return "Costumes";
    auto key = folded(item.key);
    if (key.starts_with("own_")) key.erase(0, 4);
    const auto family = family_of(key);
    if (slot_of(family) == slot && std::count(key.begin(), key.end(), '_') >= 2) {
        // Some glasses are only called shades in their title.
        if (slot == Slot::eyewear && eyewear(Words(item.title)) == "Sunglasses") return "Sunglasses";
        // "own_topshirt_<brand>_<cut>_00001": the cut follows the brand.
        const auto rest = key.size() > family.size() ? key.substr(family.size() + 1) : std::string{};
        const auto brand = rest.find('_');
        const auto cut = brand == std::string::npos ? rest : rest.substr(brand + 1);
        if (const auto kind = by_name(slot, family, cut); !kind.empty()) return kind;
        if (const auto kind = rules(slot, Words(item.title)); !kind.empty()) return kind;
        return "Other";
    }
    std::string shape_family, cut;
    if (!item.shape.empty() && shape_words(item.shape, shape_family, cut))
        if (const auto kind = by_name(slot, shape_family, cut); !kind.empty()) return kind;
    if (const auto kind = rules(slot, Words(item.title)); !kind.empty()) return kind;
    if (const auto kind = rules(slot, Words(key)); !kind.empty()) return kind;
    return "Other";
}

// Every item's kind, among the kinds of the slot the grid shows.
inline void assign_kinds(std::span<Item> items) {
    const auto slot = grid_slot(items);
    for (auto& item : items) item.kind = kind_in(slot, item);
}

// The tabs a grid gets: its items' kinds in kind_names' order, each with how many of its items
// there are and how many of those the search and the filter leave. The tile that takes an accessory
// off (no item) belongs to none.
struct KindCount {
    std::string_view name;
    unsigned total{}, shown{};
};

// Every word typed must be in the item's name. From three letters on a word may also be in the item's
// asset name, where the brand of a stock item is ("Own_ShoeSneaker_Vans_..."); shorter ones would match
// the "Own_" every asset name starts with.
inline bool matches(const Item& item, std::string_view search) {
    const auto title = folded(item.title);
    auto key = folded(item.key);
    if (key.starts_with("own_")) key.erase(0, 4);
    std::replace(key.begin(), key.end(), '_', ' ');
    const auto text = folded(search);
    for (std::size_t at = 0; at < text.size();) {
        const auto end = std::min(text.find(' ', at), text.size());
        const std::string_view word(text.data() + at, end - at);
        at = end + 1;
        if (word.empty()) continue;
        if (title.find(word) == std::string::npos && (word.size() < 3 || key.find(word) == std::string::npos)) return false;
    }
    return true;
}

// The game lists the item being worn first and the rest by name. When the first tile is out of that
// order it is the worn one, and it keeps its place ahead of the favorites.
inline bool worn_first(std::span<const Item> native) {
    return native.size() >= 2 && folded(native[0].title) > folded(native[1].title);
}

// How many tiles at the front of a grid show no item: the "none" tile that takes an accessory off.
inline unsigned leading_blanks(std::span<const Item> native) {
    unsigned count = 0;
    while (count < native.size() && native[count].key.empty()) ++count;
    return count;
}

inline bool passes(const Item& item, const std::set<std::string, std::less<>>& favorites, std::string_view search, Filter filter) {
    if (filter == Filter::favorites && !favorites.contains(item.key)) return false;
    if (filter == Filter::mods && !item.modded) return false;
    if (filter == Filter::game && item.modded) return false;
    return matches(item, search);
}

inline AccessoryRows accessory_rows(std::span<const Item> native, const AccessoryPicks* picks) {
    AccessoryRows rows;
    // fit28: the picked place's row with the picked set and sound, else the nearest installed one there (the model's
    // own set, the item with sound)
    std::map<std::string, int, std::less<>> rank;
    const bool quiet = picked_quiet(picks);
    for (unsigned i = 0; i < native.size(); ++i) {
        const auto v = accessory_variant(native[i].key);
        if (!v) continue;
        rows.first.try_emplace(v->model, i);
        if (v->place != picked_place(*v, picks)) continue;
        const auto charm = picked_charm(*v, picks);
        if ((v->charm != charm && !v->charm.empty()) || (v->quiet != quiet && v->quiet)) continue;
        const int r = (v->charm == charm ? 2 : 0) + (v->quiet == quiet ? 1 : 0);
        if (const auto at = rank.find(v->model); at == rank.end() || r > at->second) {
            rank[v->model] = r;
            rows.picked[v->model] = i;
        }
    }
    return rows;
}
// The row native row `i` is shown as (see AccessoryRows), or none.
inline std::optional<unsigned> tile_row(std::span<const Item> native, const AccessoryRows& rows, unsigned i) {
    const auto v = accessory_variant(native[i].key);
    if (!v) return i;
    const auto first = rows.first.find(v->model);
    if (first == rows.first.end() || first->second != i) return std::nullopt;
    const auto picked = rows.picked.find(v->model);
    return picked == rows.picked.end() ? i : picked->second; // that place not installed: the first row stands in
}
// Tiles in a grid: one per accessory model, one per other item.
inline unsigned tile_count(std::span<const Item> native, const AccessoryPicks* picks = nullptr) {
    const auto rows = accessory_rows(native, picks);
    unsigned count = 0;
    for (unsigned i = 0; i < native.size(); ++i) count += !native[i].key.empty() && tile_row(native, rows, i).has_value();
    return count;
}

inline std::vector<KindCount> kinds(std::span<const Item> native, const std::set<std::string, std::less<>>& favorites,
                                    std::string_view search, Filter filter, const AccessoryPicks* picks = nullptr) {
    const auto rows = accessory_rows(native, picks);
    std::vector<KindCount> result;
    for (const auto name : kind_names) {
        KindCount count{name};
        for (unsigned i = 0; i < native.size(); ++i) {
            const auto& item = native[i];
            if (item.key.empty() || item.kind != name) continue;
            const auto row = tile_row(native, rows, i);
            if (!row) continue;
            ++count.total;
            count.shown += passes(native[*row], favorites, search, filter);
        }
        if (count.total) result.push_back(count);
    }
    return result;
}

// Indices into `native` (the game's own order): the tiles that show no item (they stay where they
// are, in front, whatever is searched for), the worn item, the favorites, then everything else,
// each kept in the game's order, less what the search, the filter and the kind (empty: any) leave
// out. Never empty for a grid that has tiles: a grid with nothing in it cannot take the focus back.
inline std::vector<unsigned> arrange(std::span<const Item> native, const std::set<std::string, std::less<>>& favorites,
                                     std::string_view search, Filter filter, std::string_view kind = {},
                                     const AccessoryPicks* picks = nullptr) {
    const auto shown = [&](const Item& item) {
        if (!kind.empty() && item.kind != kind) return false;
        return passes(item, favorites, search, filter);
    };
    // fit18 / fit19: one tile per accessory model, where its first row is, showing its picked place's row
    const auto rows = accessory_rows(native, picks);
    std::vector<unsigned> result;
    result.reserve(native.size());
    const unsigned blanks = leading_blanks(native);
    for (unsigned i = 0; i < blanks; ++i) result.push_back(i);
    const unsigned first = blanks + (worn_first(native.subspan(blanks)) ? 1 : 0);
    if (first > blanks)
        if (const auto row = tile_row(native, rows, blanks); row && shown(native[*row])) result.push_back(*row);
    for (unsigned pass = 0; pass < 2; ++pass)
        for (unsigned i = first; i < native.size(); ++i)
            if (const auto row = tile_row(native, rows, i);
                row && favorites.contains(native[*row].key) == (pass == 0) && shown(native[*row]))
                result.push_back(*row);
    if (result.empty() && !native.empty()) result.push_back(0);
    return result;
}

// A row of a grid's list put back together by keep_hidden: one of the game's new rows, or one of ours.
struct KeptRow {
    bool from_game{};
    unsigned index{}; // into the game's rows, or into `native`
};

// The game changed the rows of a grid while it showed only some of them (`shown`: their item models), a
// search, a filter or a type tab leaving the rest out. A list the game builds itself (the screen opened,
// another slot, mods changed) has every item; rows holding only items that were being shown are that
// shorter list, sorted or made again by the game, and taking them for the whole list would lose the
// hidden items until the next level. Returns the whole list again: the game's rows for the items it
// has, `native`'s for the hidden ones, in `native`'s order, except that an item the game sorted to the
// front out of its titles' order is the one worn now and goes in front (the one worn before goes back
// among the others by its title). Empty: the rows are the game's own whole list.
inline std::vector<KeptRow> keep_hidden(std::span<const Item> native, std::span<const std::uint64_t> shown,
                                        std::span<const Item> rows) {
    if (native.empty() || shown.size() >= native.size()) return {};
    std::set<std::string_view> showing;
    std::vector<std::string_view> shown_order; // the keys as they were shown
    for (const auto model : shown)
        for (const auto& item : native)
            if (item.content == model && !item.key.empty()) { showing.insert(item.key); shown_order.push_back(item.key); break; }
    std::vector<std::pair<std::string_view, unsigned>> game; // the game's keyed rows, in its order
    std::vector<unsigned> game_blanks;
    for (unsigned j = 0; j < rows.size(); ++j) {
        if (rows[j].key.empty()) { game_blanks.push_back(j); continue; }
        if (!showing.contains(rows[j].key)) return {};
        game.emplace_back(rows[j].key, j);
    }
    if (game.empty()) return {};
    std::vector<KeptRow> result;
    result.reserve(native.size());
    std::size_t blanks = 0;
    for (unsigned i = 0; i < native.size(); ++i) {
        if (native[i].key.empty()) {
            result.push_back(blanks < game_blanks.size() ? KeptRow{true, game_blanks[blanks]} : KeptRow{false, i});
            ++blanks;
            continue;
        }
        const auto found = std::find_if(game.begin(), game.end(), [&](const auto& row) { return row.first == native[i].key; });
        result.push_back(found != game.end() ? KeptRow{true, found->second} : KeptRow{false, i});
    }
    // Only when the game sorted the rows itself: in the order they were shown, a favorite may well lead.
    const bool sorted = game.size() != shown_order.size() ||
        !std::equal(game.begin(), game.end(), shown_order.begin(), [](const auto& row, std::string_view key) { return row.first == key; });
    if (sorted && game.size() >= 2 && folded(rows[game[0].second].title) > folded(rows[game[1].second].title)) {
        const auto title = [&](const KeptRow& row) { return folded(row.from_game ? rows[row.index].title : native[row.index].title); };
        const auto keyed = [&](const KeptRow& row) { return !(row.from_game ? rows[row.index].key : native[row.index].key).empty(); };
        const auto front = static_cast<std::size_t>(std::find_if(result.begin(), result.end(), keyed) - result.begin());
        const auto worn = std::find_if(result.begin(), result.end(), [&](const KeptRow& row) { return row.from_game && row.index == game[0].second; });
        const auto at = static_cast<std::size_t>(worn - result.begin());
        if (at < result.size() && at != front) {
            const bool was_worn = front + 1 < result.size() && title(result[front]) > title(result[front + 1]);
            std::rotate(result.begin() + static_cast<std::ptrdiff_t>(front), worn, worn + 1);
            if (was_worn) {
                const auto before = result[front + 1];
                result.erase(result.begin() + static_cast<std::ptrdiff_t>(front + 1));
                const auto place = std::find_if(result.begin() + static_cast<std::ptrdiff_t>(front + 1), result.end(),
                    [&](const KeptRow& row) { return title(row) > title(before); });
                result.insert(place, before);
            }
        }
    }
    return result;
}
} // namespace dingosdk::item_browser
