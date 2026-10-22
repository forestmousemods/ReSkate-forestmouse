// Offline tests of the item grids' type tabs (item_browser_order.h). With a file of "key<TAB>title" lines
// as the argument it also prints how every item in it is sorted (each among its own slot's kinds).
#include "Extension/Customization/item_browser_order.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <string>

using namespace dingosdk::item_browser;
namespace {
int failures = 0, checks = 0;
void expect_in(Slot slot, std::string_view key, std::string_view title, std::string_view shape, std::string_view wanted) {
    ++checks;
    Item item{std::string(key), std::string(title)};
    item.shape = shape;
    const auto kind = kind_in(slot, item);
    if (kind != wanted) {
        ++failures;
        std::printf("FAIL %.*s (%.*s, %.*s): %.*s, wanted %.*s\n", int(key.size()), key.data(), int(title.size()), title.data(),
                    int(shape.size()), shape.data(), int(kind.size()), kind.data(), int(wanted.size()), wanted.data());
    }
}
// A game item, in its own slot.
void expect(std::string_view key, std::string_view title, std::string_view wanted) {
    auto folded_key = folded(key);
    if (folded_key.starts_with("own_")) folded_key.erase(0, 4);
    const Item item{std::string(key)};
    expect_in(grid_slot(std::span<const Item>(&item, 1)), key, title, {}, wanted);
}
void expect(bool ok, const char* what) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL %s\n", what); }
}
Item make(std::string key, std::string title = {}, std::string shape = {}) {
    Item item{std::move(key), std::move(title)};
    item.shape = std::move(shape);
    return item;
}
} // namespace

int main(int argc, char** argv) {
    // Game items: the cut decides, in the right order.
    expect("own_topshirt_thrasher_tshirtrelaxed_00004", "Thrasher Mag Navy Tee", "T-Shirts");
    expect("own_topshirt_gen_hoodierelaxed_00001", "", "Hoodies");
    expect("own_topshirt_gen_tshirtoverlongsleeve_00001", "", "Longsleeves");
    expect("own_topshirt_gen_tshirtsleeveless_00001", "", "Tanks");
    expect("own_topshirt_gen_cropcrewneck_00001", "", "Crop Tops");
    expect("own_topshirt_gen_camicropribbed_00001", "", "Crop Tops");
    expect("own_topshirt_gen_sweatshirtcrewneck_00001", "", "Sweatshirts");
    expect("own_topshirt_gen_fleecequarterzip_00001", "", "Sweatshirts");
    expect("own_topshirt_gen_buttonupshortsleeve_00001", "Cowboy Button Up", "Shirts");
    expect("own_topshirt_gen_camp_00001", "", "Shirts");
    expect("own_topshirt_gen_polobaggy_00001", "", "Shirts");
    expect("own_topshirt_gen_raglan3qrtrsleeve_00001", "", "Longsleeves");
    expect("own_topjacket_adidas_trackzipped_00003", "Kogan Track Jacket", "Jackets");
    expect("own_topjacket_gen_hoodietechfleece_00001", "", "Hoodies");
    expect("own_topjacket_gen_denimsleeveless_00002", "Black Ripped Denim Vest", "Jackets");
    expect("own_topvest_gen_puffertechbro_00001", "", "Jackets");
    expect("own_topcoat_gen_pufferoversize_00001", "", "Jackets");
    expect("own_topsweater_gen_crewneckfrayed_00002", "Autumn Sweater", "Sweatshirts");
    expect("own_topbra_gen_sportsbanded_00001", "", "Crop Tops");
    expect("own_topcostume_gen_jacketdemon_00001", "", "Costumes");
    expect("own_bottompants_vans_jeansbaggy_00005", "Vans x skate. Jeans", "Jeans");
    expect("own_bottompants_gen_dressstraightbelted_00001", "", "Pants");
    expect("own_bottompants_dickies_874regular_00001", "", "Chinos");
    expect("own_bottompants_gen_joggertechfleece_00001", "", "Track Pants");
    expect("own_bottompants_gen_leggingsmidwaist_00001", "", "Leggings");
    expect("own_bottomshorts_gen_baggydenim_00001", "", "Shorts");
    expect("own_bottomskirt_gen_pleatedlowwaist_00001", "", "Skirts");
    expect("own_shoesneaker_vans_skatesk8hi_00001", "", "High Tops");
    expect("own_shoesneaker_vans_skatehalfcab_00003", "Vans Half Cab", "High Tops");
    expect("own_shoesneaker_nike_sbdunklowpro_00003", "Nike SB Dunk Low Pro", "Low Tops");
    expect("own_shoesneaker_adidas_freehiker3lowgtx_00001", "Free Hiker", "Low Tops");
    expect("own_shoesneaker_vans_sliponskateboarding_00001", "", "Slip-Ons");
    expect("own_shoeflat_gen_clogkitchen_00004", "Goth Clog", "Flats");
    expect("own_headwearhat_gen_ballcap6panel_00020", "", "Caps");
    expect("own_headwearhat_gen_beaniewatch_00001", "", "Beanies");
    expect("own_headwearhat_gen_hatbucket_00001", "", "Bucket Hats");
    expect("own_headwearhelmet_gen_helmetskateboard_00001", "", "Helmets");
    expect("own_eyewearglasses_gen_sunglassespilot_00004", "", "Sunglasses");
    expect("own_eyewearglasses_gen_eyeglasseshalfrim_00002", "", "Glasses");
    expect("own_sock_gen_kneehighmedium_00009", "", "Knee High");
    expect("own_outfitoveralls_gen_overallsboilersuit_00005", "Prison Jumpsuit", "Overalls");
    expect("own_outfitdress_gen_chorehalfplacket_00004", "", "Dresses");
    expect("own_costume_gen_sumo_00001", "Sumo Costume", "Costumes");

    // Mods' items: only their slot's kinds; the game's shape they were made on, else whole words.
    expect_in(Slot::tops, "Own_CSPFW", "Carti Supreme Patchwork FW16", "Gen_Shirt_HoodieRelaxed_complex_dmPreset", "Hoodies");
    expect_in(Slot::tops, "Own_CSPFW", "Carti Supreme Patchwork FW16", "", "Other"); // was "Chinos" (patchWORK)
    expect_in(Slot::tops, "Own_BLD_PCK_GCM", "BLD PCK Gucci Chateau Marmont", "Gen_Shirt_TshirtRelaxed_complex_dmPreset", "T-Shirts");
    expect_in(Slot::tops, "Own_BLD_PCK_GCM", "BLD PCK Gucci Chateau Marmont", "", "Other"); // was "Hats" (cHATeau)
    expect_in(Slot::tops, "own_01_pantera_skull_t_shirt", "01 Pantera Skull T Shirt", "", "T-Shirts"); // was "Pants"
    expect_in(Slot::tops, "Own_NSOLSSB", "Nike Short Over Long Sleeve Shirt Black", "", "Longsleeves"); // was "Shorts"
    expect_in(Slot::tops, "Own_ACP", "Arctic Camo Puffer", "Gen_Coat_PufferOversize_complex_dmPreset", "Jackets");
    expect_in(Slot::tops, "Own_CHBA_J", "Carti HBA Jacket", "471aadc0-8f24-4c56-8c96-d79ff2b67416_dmpreset", "Jackets");
    expect_in(Slot::tops, "Own_ATHB", "Aphex Twin Hoodie Black", "_copy_f6551e1b-c09b-49f1-aae3-e33cfe93fa19_10_dmpreset", "Hoodies");
    expect_in(Slot::tops, "Own_BBU", "Brains Button Up", "", "Shirts");
    expect_in(Slot::tops, "Own_AcidIBFTT", "AcidInvert Black Fitted Tank", "", "Tanks");
    expect_in(Slot::tops, "Own_HUQ", "Hook-Ups Quarter Zip", "", "Sweatshirts");
    expect_in(Slot::tops, "Own_PP", "Primitive Hoodie", "Primitive_Shirt_HoodieRelaxed_complex_dmPreset", "Hoodies");
    expect_in(Slot::tops, "Own_X", "Chains Logo", "", "Other"); // no "Chains" among tops
    expect_in(Slot::bottoms, "Own_FJ01", "fleece jogger 01", "", "Track Pants"); // was "Crop Tops"
    expect_in(Slot::bottoms, "Own_PPDJ", "Pink Panther Denim Jorts", "", "Shorts");
    expect_in(Slot::bottoms, "Own_CDK", "Carhartt Double Knee", "", "Chinos");
    expect_in(Slot::bottoms, "Own_LB", "194 Local Baggies", "Gen_Pants_JeansBaggy_complex_dmPreset", "Jeans");
    expect_in(Slot::bottoms, "Own_SBS", "Supreme Black Shorts", "", "Shorts");
    expect_in(Slot::shoes, "Own_DH", "Nike SB Dunk High Assortment", "", "High Tops");
    expect_in(Slot::shoes, "Own_VOS", "Vans Old Skool Black and White", "Vans_Sneaker_SkateOldSkool_complex_dmPreset", "Low Tops");
    expect_in(Slot::shoes, "Own_TN", "Nike Tuned TN", "", "Low Tops");
    expect_in(Slot::headwear, "Own_CHDH", "Carti Horned Dunce Hat", "", "Hats");
    expect_in(Slot::outfits, "own_lanyard_thrasher_black_front_right", "Lanyard Thrasher Black Front Right", "", "Lanyards");
    expect_in(Slot::outfits, "own_keychain_cross_black_side", "", "", "Keychains");
    expect_in(Slot::outfits, "own_wallet_chain_silver_front", "", "", "Chains");
    expect_in(Slot::tops, "Own_TopB", "ownbra2", "Gen_Bra_SportsBanded_complex_dmPreset", "Crop Tops");
    expect_in(Slot::tops, "Own_CHHZBC", "Chrome Hearts HoodieZip Black Cross", "", "Hoodies");
    expect_in(Slot::tops, "Own_GPBU", "Green Plaid Butn Up", "", "Shirts");
    expect_in(Slot::socks, "Own_TBS", "Thighhigh Black Stripes", "", "Thigh High");
    expect_in(Slot::headwear, "Own_B", "Beanie8", "", "Beanies");
    expect_in(Slot::headwear, "Own_BWSCTH", "Blood Wizard Spell Caster Truckhat", "", "Caps");
    expect_in(Slot::bottoms, "Own_CNP", "Carhartt Nolan Pants5", "", "Pants");
    expect_in(Slot::shoes, "Own_VX_CO", "Vans x Creature OldSkool", "", "Low Tops");
    expect_in(Slot::tops, "Own_short_sleeve_01", "short sleeve 01", "c2d1dbce-992b-482d-bcc6-326627f2df49_dmpreset", "T-Shirts");
    expect_in(Slot::shoes, "Own_gat", "gat", "b42cd19d-8f53-47ad-8eb7-b36820fa2cfb_geometry", "Low Tops");
    expect_in(Slot::tops, "", "", "", "");

    // A grid's slot is what most of its game items are; its tabs are that slot's kinds only.
    std::vector<Item> grid{make(""), make("own_topshirt_a_tshirtrelaxed_1"), make("own_topshirt_a_hoodierelaxed_1", "Red Hoodie"),
                           make("own_topshirt_b_tshirtrelaxed_2"), make("own_topjacket_c_bomberclassic_1"),
                           make("Own_PJ", "Pantera Jeans Tee")};
    expect(grid_slot(grid) == Slot::tops, "grid slot by majority");
    assign_kinds(grid);
    expect(grid[5].kind == "T-Shirts", "a mod's item among the grid's kinds");
    const std::set<std::string, std::less<>> none, favorite{"own_topshirt_b_tshirtrelaxed_2"};
    const auto tabs = kinds(grid, none, "", Filter::all);
    expect(tabs.size() == 3 && tabs[0].name == "T-Shirts" && tabs[0].total == 3 && tabs[1].name == "Hoodies" &&
           tabs[2].name == "Jackets", "kinds in table order, the blank tile in none");
    const auto liked = kinds(grid, favorite, "", Filter::favorites);
    expect(liked[0].total == 3 && liked[0].shown == 1 && liked[1].shown == 0, "kind counts follow the filter");
    // Our own Accessories slot's grid holds only our items: it is an outfits grid, with their kinds as tabs.
    std::vector<Item> accessories{make(""), make("Own_Keychain_Angular_Navy_Front", "Keychain Angular Navy Front"),
                                  make("Own_Lanyard_Supreme_Side", "Lanyard Supreme Side"),
                                  make("Own_Wallet_Chain_Gold_Back", "Wallet Chain Gold Back"),
                                  make("Own_Keychain_Finish_Test", "Keychain Finish Test")};
    expect(grid_slot(accessories) == Slot::outfits, "the accessories grid is an outfits grid");
    assign_kinds(accessories);
    expect(accessories[1].kind == "Keychains" && accessories[2].kind == "Lanyards" && accessories[3].kind == "Chains" &&
           accessories[4].kind == "Keychains", "our accessories' kinds in their own grid");
    const auto red = kinds(grid, none, "red", Filter::all);
    expect(red[1].shown == 1 && red[0].shown == 0, "kind counts follow the search");
    const auto shirts = arrange(grid, none, "", Filter::all, "T-Shirts");
    expect(shirts == std::vector<unsigned>{0, 1, 3, 5}, "a kind keeps the blank tile and its own items");
    expect(arrange(grid, none, "", Filter::all).size() == grid.size(), "no kind: everything");
    std::vector<Item> single{make("own_topshirt_a_tshirtrelaxed_1")};
    assign_kinds(single);
    expect(arrange(single, none, "", Filter::all, "Hoodies") == std::vector<unsigned>{0}, "never empty");

    // The game remade a filtered list: the hidden items come back, the remade ones are the game's.
    {
        const auto item = [](std::string key, std::string title, std::uint64_t model) {
            auto made = make(std::move(key), std::move(title));
            made.content = model;
            return made;
        };
        // Worn "Zed" first, then by title: blank, Zed, Alpha, Bravo, Charlie, Delta.
        const std::vector<Item> native{item("", "", 1), item("k_zed", "Zed", 2), item("k_alpha", "Alpha", 3),
                                       item("k_bravo", "Bravo", 4), item("k_charlie", "Charlie", 5), item("k_delta", "Delta", 6)};
        const std::vector<std::uint64_t> shown{1, 3, 5}; // a tab: blank, Alpha, Charlie
        const auto order = [](const std::vector<KeptRow>& rows) {
            std::string out;
            for (const auto& row : rows) out += (row.from_game ? "g" : "n") + std::to_string(row.index) + " ";
            return out;
        };
        // The same items in new models, in the order they were shown.
        const std::vector<Item> same{item("", "", 11), item("k_alpha", "Alpha", 13), item("k_charlie", "Charlie", 15)};
        expect(order(keep_hidden(native, shown, same)) == "g0 n1 g1 n3 g2 n5 ", "remade rows keep the hidden ones");
        // The game sorted them again with Charlie worn now: Charlie in front, Zed back among the others.
        const std::vector<Item> worn{item("", "", 11), item("k_charlie", "Charlie", 15), item("k_alpha", "Alpha", 13)};
        expect(order(keep_hidden(native, shown, worn)) == "g0 g1 g2 n3 n5 n1 ", "the newly worn item goes in front");
        // An item that was hidden: the game's own whole list.
        const std::vector<Item> whole{item("", "", 11), item("k_bravo", "Bravo", 14), item("k_alpha", "Alpha", 13)};
        expect(keep_hidden(native, shown, whole).empty(), "a hidden item in the rows: the game's list");
        // Nothing hidden: whatever the game has is its list.
        const std::vector<std::uint64_t> all{1, 2, 3, 4, 5, 6};
        expect(keep_hidden(native, all, same).empty(), "nothing hidden: the game's list");
        // Only blank tiles: not ours to judge.
        const std::vector<Item> blank{item("", "", 11)};
        expect(keep_hidden(native, shown, blank).empty(), "no items: the game's list");
        // A favorite led the shown order (Delta before Alpha): kept as it was, nobody is newly worn.
        const std::vector<std::uint64_t> led{1, 6, 3};
        const std::vector<Item> as_shown{item("", "", 11), item("k_delta", "Delta", 16), item("k_alpha", "Alpha", 13)};
        expect(order(keep_hidden(native, led, as_shown)) == "g0 n1 g2 n3 n4 g1 ", "shown order is not a new worn item");
    }

    // A scan of installed mods (mod<TAB>cust_slot<TAB>key<TAB>name<TAB>shapes<TAB>looks): how their items sort.
    if (argc > 1 && std::string_view(argv[1]).ends_with(".tsv")) {
        const std::map<std::string, Slot> slots{{"cust_tops", Slot::tops}, {"cust_bottoms", Slot::bottoms},
            {"cust_shoes", Slot::shoes}, {"cust_hats", Slot::headwear}, {"cust_eyewear", Slot::eyewear},
            {"cust_socks", Slot::socks}, {"cust_outfits", Slot::outfits}};
        std::ifstream in(argv[1]);
        std::map<std::string, std::map<std::string, int>> sorted;
        std::vector<std::string> others;
        for (std::string line; std::getline(in, line);) {
            std::vector<std::string> f;
            for (std::size_t at = 0; at <= line.size();) {
                const auto end = std::min(line.find('\t', at), line.size());
                f.push_back(line.substr(at, end - at));
                at = end + 1;
            }
            if (f.size() < 5 || !slots.contains(f[1])) continue;
            auto title = f[3].starts_with("own_") ? f[3].substr(4) : f[3];
            std::replace(title.begin(), title.end(), '_', ' ');
            auto item = make(f[2], title, f[4].substr(0, f[4].find(';')));
            const auto kind = std::string(kind_in(slots.at(f[1]), item));
            ++sorted[f[1]][kind];
            if (kind == "Other") others.push_back(f[1] + " | " + f[2] + " | " + title + " | " + item.shape);
        }
        for (const auto& [slot, counts] : sorted) {
            std::printf("%s:", slot.c_str());
            for (const auto& [kind, count] : counts) std::printf(" %s %d,", kind.c_str(), count);
            std::printf("\n");
        }
        if (argc > 2) for (const auto& other : others) std::printf("    other: %s\n", other.c_str());
    } else if (argc > 1) {
        std::ifstream in(argv[1]);
        std::map<std::string, std::map<std::string, int>> sorted; // family -> kind -> count
        std::map<std::string, std::vector<std::string>> others;
        std::set<std::string> seen;
        for (std::string line; std::getline(in, line);) {
            const auto tab = line.find('\t');
            const Item item{line.substr(0, tab), tab == std::string::npos ? std::string{} : line.substr(tab + 1)};
            const auto kind = std::string(kind_in(grid_slot(std::span<const Item>(&item, 1)), item));
            const auto key = folded(item.key);
            const auto family = key.substr(4, key.find('_', 4) - 4);
            ++sorted[family][kind];
            if (kind == "Other") others[family].push_back(item.key + " | " + item.title);
            const auto cut = key.substr(0, key.find_last_of('_'));
            if (argc > 2 && seen.insert(cut + kind).second) std::printf("  %-55s %s\n", cut.c_str(), kind.c_str());
        }
        for (const auto& [family, counts] : sorted) {
            std::printf("%s:", family.c_str());
            for (const auto& [kind, count] : counts) std::printf(" %s %d,", kind.c_str(), count);
            std::printf("\n");
            for (const auto& other : others[family]) std::printf("    other: %s\n", other.c_str());
        }
    }
    {   // fit18: one tile per accessory model
        const auto v = accessory_variant("Own_Keychain_Heartagram_Chain_Black_Front_Clip_Right");
        expect(v && v->kind == AccessoryKind::keychain && v->model == "own_keychain_heartagram_chain_black" &&
                   v->place == "_front_clip_right" && !v->shown, "keychain variant");
        const auto b = accessory_variant("own_keychain_heartagram_chain");
        expect(b && b->place.empty() && b->shown && b->model == "own_keychain_heartagram_chain", "keychain tile");
        const auto r = accessory_variant("own_keychain_carabiner_camo_right");
        expect(r && r->place == "_right" && r->model == "own_keychain_carabiner_camo", "keychain back right");
        const auto l = accessory_variant("own_lanyard_thrasher_white_side_right");
        expect(l && l->kind == AccessoryKind::lanyard && l->model == "own_lanyard_thrasher_white" && l->place == "_side_right",
               "lanyard variant");
        expect(!accessory_variant("own_lanyard_thrasher_white_front_clip") ||
                   accessory_variant("own_lanyard_thrasher_white_front_clip")->place != "_front_clip", "lanyards have no clip");
        const auto w = accessory_variant("Own_Wallet_Chain_Ball_Front");
        expect(w && w->kind == AccessoryKind::wallet && w->shown && w->model == "own_wallet_chain_ball", "wallet tile");
        const auto wb = accessory_variant("own_wallet_chain_gold_back_right");
        expect(wb && !wb->shown && wb->place == "_back_right" && wb->model == "own_wallet_chain_gold", "wallet variant");
        expect(!accessory_variant("own_wallet_chain_gold"), "wallet without a place");
        expect(!accessory_variant("Own_TopShirt_Front"), "not ours");
        expect(default_place(AccessoryKind::keychain) == "_front_clip" && default_place(AccessoryKind::lanyard).empty() &&
                   default_place(AccessoryKind::wallet) == "_front", "default places");
        expect(strap_toggled("_side_right") == "_front_clip_right" && strap_toggled("_front_clip") == "_front" &&
                   strap_toggled("") == "_front_clip", "strap toggle");
        expect(place_group(AccessoryKind::keychain, "_front_clip").size() == 2 &&
                   place_group(AccessoryKind::keychain, "_side").size() == 6 &&
                   place_group(AccessoryKind::wallet, "_back").size() == 4, "place groups");
        const std::vector<Item> tiles{make("", ""), make("own_keychain_cross_black", "Keychain Cross Black"),
                                     make("own_keychain_cross_black_front", "Keychain Cross Black Front"),
                                     make("own_keychain_cross_black_front_clip", "Keychain Cross Black Front Clip"),
                                     make("own_lanyard_palace", "Lanyard Palace"),
                                     make("own_lanyard_palace_front", "Lanyard Palace Front"),
                                     make("own_topshirt_x", "T")};
        const auto plain = arrange(tiles, {}, "", Filter::all);
        expect(plain == std::vector<unsigned>{0, 3, 4, 6}, "grid: one tile per model, keychains by their clip");
        AccessoryPicks picks{{"own_keychain_cross_black", "_front"}, {"own_lanyard_palace", "_front"}};
        const auto picked = arrange(tiles, {}, "", Filter::all, {}, &picks);
        expect(picked == std::vector<unsigned>{0, 2, 5, 6}, "grid: the picked places, where the models were");
        expect(tile_count(tiles) == 3, "tile count");
        // a model whose first row comes after another model's (Heartagram Chain / ... Black): its tile stays there
        const std::vector<Item> chains{make("own_keychain_heartagram_chain", "Keychain Heartagram Chain"),
                                       make("own_keychain_heartagram_chain_black_front_clip", "Keychain Heartagram Chain Black Front Clip"),
                                       make("own_keychain_heartagram_chain_front_clip", "Keychain Heartagram Chain Front Clip")};
        expect(arrange(chains, {}, "", Filter::all) == std::vector<unsigned>{2, 1}, "tile keeps its model's first place");
    }
    {   // fit28: charm sets and quiet twins on the model's tile
        const auto k = accessory_variant("Own_Keychain_Angular_Navy_Keys_Quiet_Front_Clip_Right");
        expect(k && k->model == "own_keychain_angular_navy" && k->charm == "_keys" && k->quiet &&
                   k->place == "_front_clip_right", "charm set + quiet twin");
        const auto b = accessory_variant("own_keychain_angular_navy_ball");
        expect(b && b->model == "own_keychain_angular_navy" && b->charm == "_ball" && !b->quiet && b->place.empty(), "set at the back");
        const auto w = accessory_variant("own_wallet_chain_ball_front");
        expect(w && w->model == "own_wallet_chain_ball" && w->charm.empty(), "wallet chains have no charm sets");
        const auto l = accessory_variant("own_lanyard_palace_quiet_side");
        expect(l && l->model == "own_lanyard_palace" && l->quiet && l->place == "_side", "quiet lanyard");
        expect(variant_key("own_keychain_bent_red", "_board", true, "_side") == "own_keychain_bent_red_board_quiet_side", "key");
        const std::vector<Item> tiles{make("", ""), make("own_keychain_angular_navy_front_clip", "A"),
                                     make("own_keychain_angular_navy_keys_front_clip", "A keys"),
                                     make("own_keychain_angular_navy_keys", "A keys back"),
                                     make("own_keychain_angular_navy_quiet_front_clip", "A quiet")};
        expect(arrange(tiles, {}, "", Filter::all) == std::vector<unsigned>{0, 1}, "own set, with sound, by default");
        AccessoryPicks keys{{charm_pick("own_keychain_angular_navy"), "_keys"}};
        expect(arrange(tiles, {}, "", Filter::all, {}, &keys) == std::vector<unsigned>{0, 2}, "the keys set picked");
        AccessoryPicks quiet{{std::string(sound_pick), "off"}};
        expect(arrange(tiles, {}, "", Filter::all, {}, &quiet) == std::vector<unsigned>{0, 4}, "quiet picked");
        AccessoryPicks both{{charm_pick("own_keychain_angular_navy"), "_keys"}, {std::string(sound_pick), "off"}};
        expect(arrange(tiles, {}, "", Filter::all, {}, &both) == std::vector<unsigned>{0, 2}, "keys, no quiet twin of it: keys");
        AccessoryPicks back{{"own_keychain_angular_navy", ""}, {charm_pick("own_keychain_angular_navy"), "_ball"}};
        expect(arrange(tiles, {}, "", Filter::all, {}, &back) == std::vector<unsigned>{0, 1},
               "no item at the place picked: the first row stands in");
    }
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
