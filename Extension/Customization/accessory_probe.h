#pragma once
#include <cstddef>
#include <cstdint>
#include <array>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// Research aid for the accessory customizer (Physical_Lanyards keychains, 2026-10-16): can our
// items be coloured, and moved, while the game runs? Three probes, each with a result one can see:
// the item in the Outfits slot takes a colour; the bones of our renderbones asset are found in the
// skater's skinning palette and lifted; the palettes are written to logs\accessory_probe for study.
// Console: acc color <n>, acc lift <cm>, acc mode 1|2, acc dump, acc status.
namespace dingosdk::accessory_probe {
// The accessory fit (customizer v2): where a keychain hangs at each of its 8 places and a lanyard at each of its 6, in
// cm from the place the mod gives it: up, out (away from the leg) and along (back / front: towards the side; side:
// forward); turned about its hanging line (turn, degrees). Kept per pants. `tilt` (fit16) stays in the saved text but
// is always 0 since fit17: a hanging part leaned against gravity tore off its top ring. fit33: `coll` (cm) thickens the
// thigh capsules our accessories hang against (baggy pants: the keychain no longer sinks into them); the worn
// place's value applies to all of our capsules.
struct Offset {
    float up{}, out{}, along{}, turn{}, tilt{}, coll{};
    bool operator==(const Offset&) const = default;
};
struct FitView {
    int place = -1;              // the worn accessory's place: keychains 0 back, 1 front, 2 side, 3-5 the same on the right,
                                 // 6 / 7 front clip left / right; lanyards 8 back, 9 front, 10 side, 11-13 right; -1: none
    std::string outfit, pants;   // item keys
    Offset offset;               // the place's offset in force
    bool pants_own{};            // these pants have their own fit (else the one for all pants)
    std::size_t live{}, lasting{};
    std::string summary;
    bool scanning{};
};
FitView fit_view() noexcept;                       // any thread
// fit18: one tile per accessory model (item_browser_order.h accessory_variant). The panel picks the model's place, kept
// per model; the skater wears the model's item at the picked place.
struct ModelPick {
    std::string model;  // lower case; empty: the key is none of ours
    int kind = -1;      // item_browser::AccessoryKind
    std::string place;  // the place picked (one of the kind's place words)
    // fit28: the charm set picked ("" = the model's own), the sets installed for the model (in charm_sets order),
    // the sound picked for all of them (quiet = the twins without our sounds) and whether the model has quiet twins
    std::string charm;
    std::vector<std::string> charms;
    bool quiet{}, has_quiet{};
    // fit47: the places the model is installed at (in the kind's place order); `place` is always one of them
    std::vector<std::string> places;
};
ModelPick model_pick(std::string_view key) noexcept;              // any thread
// fit52: how far (radians, signed) the panel's skater is turned from showing its keychain to the camera; false when
// no fresh value (the panel camera is not on)
bool panel_turn_error(float& radians) noexcept;
// fit53: which way the right stick turns the wardrobe skater (+1: the error grows with +stick; -1 the other way; 0 not
// known yet) - measured by the panel, kept in the profile ("Accessories.PanelTurn")
int panel_turn_sign() noexcept;
void set_panel_turn_sign(int sign) noexcept;
void panel_note(const std::string& line) noexcept;   // fit54: a line in the accessories log, from the panel
std::map<std::string, std::string, std::less<>> model_picks() noexcept; // every pick made (model -> place), any thread
void pick_place(std::string_view model, std::string_view place) noexcept;
void pick_charm(std::string_view model, std::string_view charm) noexcept; // fit28
void pick_sound(bool on) noexcept;                                         // fit28: all of our items
bool installed(std::string_view key) noexcept;                             // an item of ours by that key, any thread
int fit_place(int kind, std::string_view place) noexcept;         // the fit's place there (-1: wallet chains, no fit)
const char* place_name(int place) noexcept;
void set_offset(int place, Offset offset) noexcept; // saved with the profile a moment later
void set_pants_own(bool own) noexcept;
Offset place_offset(int place) noexcept;           // a place's offset in force (for the pants worn)
bool pants_own() noexcept;
bool pants_worn() noexcept;
int place_of(std::string_view key) noexcept;       // an item key's place (a keychain or lanyard of ours), -1: none
bool place_is_lanyard(int place) noexcept;
int current_finish() noexcept;
// fit34: the strap's own colour (keychains whose braid is on the second material, Outfit_Bottom_mat), same palette
void set_strap(int strap) noexcept;
int current_strap() noexcept;
bool strap_available() noexcept; // the keychain looked at last has a strap material
// fit36: the charm slots (a keychain with charm slot bones: KC40's "Keychain Charm Slots Test"): per slot 0..2 the
// option shown (-1 none), kept per keychain ("Accessories.Charms.<item>").
bool charm_slots_available() noexcept;              // the keychain worn has the slots
int charm_option_count() noexcept;
const char* charm_option_name(int option) noexcept; // "None" for -1
int current_charm(int slot) noexcept;
void set_charm(int slot, int option) noexcept;      // fit59: an option picked here leaves the other slot
void step_charm(int slot, int step) noexcept;      // fit59: the next option not hung in another slot
void prepare_fit() noexcept;                      // one of our keychains is picked: find its copies now
void set_shown_item(std::string_view key) noexcept; // fit57: the grid's focused item of ours (on the wardrobe skater)
void set_focus(int place, float aspect) noexcept;  // the camera looks at the keychain at this place for 300 ms (-1 off)
const char* finish_name(int finish) noexcept;
std::array<float, 3> finish_swatch(int finish) noexcept; // fit31: the colour a finish shows (sRGB-ish 0..1), for the panel
int finish_count() noexcept;             // own fit for the worn pants (from what is shown), or back to the shared one
int strap_count() noexcept;              // fit58: the strap's own palette
const char* strap_name(int strap) noexcept;
std::array<float, 3> strap_swatch(int strap) noexcept;
void set_color(int choice) noexcept;      // 0: the item's own; 1 purple, 2 acid green, 3 gold, 4 pink, 5 ice blue, 6 black
void set_part(int part) noexcept;          // the slot coloured: 0 top, 1 bottoms, 6 outfit (our items)
void set_region(int region) noexcept;      // -1: every colour parameter of the item, else only that one (0-based)
void set_lift(float centimetres) noexcept; // 0: none; up along the anchor's own up
void set_mode(int mode) noexcept;          // 1: our copy of the bones handed to the renderer, 2: the object's bones edited
void request_dump() noexcept;
void request_colortest() noexcept;        // one colour parameter / mode after the other, the step shown on screen
void set_finish(int finish) noexcept;      // the metal parts' finish: 0 own, 1-8 colours, 9-11 chameleons
void set_brightness(float value) noexcept; // of the finish, 0.3-1.6
void request_params() noexcept;            // logs every parameter of the top's and our item's materials
void set_colormode(int mode) noexcept;     // our item's colour mode: -1 like the top, -2 its own, else that value
void request_scan() noexcept;              // finds the keychain anchors' rest offsets in memory (background thread)
void request_status() noexcept;
// Client tick (the verified update thread), with the game's image base and client.
void tick(std::uintptr_t base, std::uintptr_t client, bool ready) noexcept;
// fit60: Physical Lanyards is applied on a PC whose Discord account (now or before) is on the mod's lock list - our
// accessory features are off and the overlay covers the screen with a notice. Any thread.
bool access_locked() noexcept;
const char* access_lock_title() noexcept;  // fit60e: the notice's title for the account locked
const char* access_lock_line() noexcept;   // fit60d: the notice's second line for the account locked ("" before)
}
