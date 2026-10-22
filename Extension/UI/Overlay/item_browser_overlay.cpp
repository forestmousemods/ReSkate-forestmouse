#include "overlay_internal.h"
#include "Engine/Core/Platform/path_text.h"
#include "input_capture.h"
#include "cursor.h"
#include "Extension/Customization/accessory_probe.h"
#include "Extension/Customization/item_browser_order.h"
#include "Engine/Core/Log/logging.h"
#include <imgui_internal.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>

// The skater item grids' search bar and key prompts, drawn over the game's own menu in its style.
// The grids themselves are the game's (Extension/Customization/item_browser.cpp); this only shows
// what is going on and takes the typing.
namespace dingosdk::overlay {
namespace {
std::atomic<const ItemBrowserHost*> item_browser_host{};
}
void set_item_browser(const ItemBrowserHost* host) noexcept { item_browser_host.store(host); }
} // namespace dingosdk::overlay

namespace dingosdk::overlay::detail {
namespace {
using Clock = std::chrono::steady_clock;
// Where the type tabs were drawn last, for the mouse.
struct TabBar {
    bool shown{};
    ImVec2 display{};                      // the screen they were laid out for
    ImVec2 a{}, b{};                       // the bar
    float previous_x0{}, previous_x1{}, next_x0{}, next_x1{}; // the LT and RT prompts
    float clip_x0{}, clip_x1{};            // the part of the bar the tabs scroll in
    std::vector<std::pair<float, float>> tabs; // each tab's left and right edge on screen
};
// The keychain panel (Extension/Customization/accessory_probe.h): on one of our keychains in a grid, R3 / V opens it
// beside the grid. While it is open it takes the pad and the keys from the game (the cards' input hold) and reads them
// itself; after it closes the hold lasts until every button is let go, so the B that closed it never reaches the game.
struct FitPanel {
    bool open{}, capture{}, release{};
    int row{}, place{-1};
    std::string model;            // fit18: the model the panel is for (its tile has the focus)
    int kind{-1};
    // fit20: the picked place's item to equip (lower case key). The game equips the item it focused, so the panel
    // moves the game's focus off the tile and back (stage 1, 2), checking each step, then taps A (stage 3).
    // fit25: each step as soon as the game shows the last one (a few frames), not after a fixed wait.
    std::string equip;
    int stage{}, home{-1}, dir{}, tries{}, last_index{-1};
    Clock::time_point equip_until{}, step_at{}, lost_since{}, equip_at{}, arrived{};
    std::string trace;            // fit26: what each step saw, logged when the equip ends
    std::uint32_t buttons{};      // last frame's pad buttons
    std::array<bool, 8> keys{};   // last frame's keys: up, down, left, right, Enter, Esc / Backspace, V, Space
    Clock::time_point repeat_at{};
    int repeat{};                 // the direction held for the auto-repeat
};
// fit18: the rows depend on the model: a keychain's strap and place, a lanyard's place, a wallet chain's place only.
enum class FitRow : std::uint8_t { strap, charm, charm1, charm2, charm3, place, up, out, along, turn, tilt, pants, finish, strapcolor, sound, reset };
// fit28: Charms when the keychain comes with more than one charm set, Sound when the model has quiet twins.
std::vector<FitRow> fit_row_list(int kind, const dingosdk::accessory_probe::ModelPick& pick) {
    using R = FitRow;
    using K = dingosdk::item_browser::AccessoryKind;
    std::vector<R> rows;
    // fit32: LOOK, then POSITION, then FIT (fit_section)
    if (kind == static_cast<int>(K::keychain)) {
        rows.push_back(R::strap);
        if (pick.charms.size() > 1) rows.push_back(R::charm);
        if (dingosdk::accessory_probe::charm_slots_available())   // fit36: Charm 1 / 2 / 3, any charm on each
            for (const auto r : {R::charm1, R::charm2, R::charm3}) rows.push_back(r);
        rows.push_back(R::finish);
        // fit34; fit55: not at the Front Clip places (the slot mesh's braid section has no braid there)
        if (dingosdk::accessory_probe::strap_available() && !pick.place.starts_with("_front_clip")) rows.push_back(R::strapcolor);
    }
    if (pick.has_quiet) rows.push_back(R::sound);
    rows.push_back(R::place);
    if (kind != static_cast<int>(K::wallet)) {
        for (const auto r : {R::up, R::out, R::along, R::turn, R::tilt}) rows.push_back(r);
        // fit36: fit33's Pants collision row and fit35's Charm slot test are gone (no visible change in the game)
        rows.push_back(R::pants), rows.push_back(R::reset);
    }
    return rows;
}
// fit32: which part of the panel a row is in (0 look, 1 position, 2 fit)
int fit_section(FitRow row) {
    switch (row) {
    case FitRow::strap: case FitRow::charm: case FitRow::charm1: case FitRow::charm2: case FitRow::charm3:
    case FitRow::finish: case FitRow::strapcolor: case FitRow::sound: return 0;
    case FitRow::pants: case FitRow::reset: return 2;
    default: return 1;
    }
}
struct Panel {
    FitPanel fit;
    float aspect = 16.0f / 9.0f;   // the screen's, from the last draw (the update runs outside the ImGui context)
    ItemBrowserView view;
    std::array<char, 96> input{};
    bool stick_down{};
    std::string pushed; // the search text the host was last given
    TabBar bar;
    bool previous_down{}, next_down{}, click_down{};
    Clock::time_point tabs_seen{}; // the tabs were last up then: LT and RT stay the tabs' a moment longer
    std::atomic<bool> pad_prompts{}; // the last input was a controller's: show LT / RT, else Z / C
    ImVec2 mouse{-1, -1};        // the pointer on the screen, while a grid is open
    float scroll{};              // how far the tabs are scrolled to the left
    Clock::time_point drawn{};
};
Panel& panel() { static auto* value = new Panel; return *value; }
const ItemBrowserHost* host() { return item_browser_host.load(); }

// The icon of the mod the highlighted item is from: read and decoded off this thread, uploaded by
// the render thread to the SRV heap's fourth slot (overlay_render.cpp, upload_mod_icon).
struct ModIcon {
    std::mutex mutex;
    std::string wanted, tried;  // the icon asked for, and the last one the loader finished
    bool loading{};
    // Icons decoded lately (a mod with none as no pixels), so going back to a mod's item shows its
    // icon at once: at most `kept`, 64 KB each.
    static constexpr std::size_t kept = 32;
    std::vector<std::pair<std::string, CardPixels>> decoded;
    CardPixels current;         // kept to upload again after a device reset
    std::string current_path;
    bool waiting{};
    unsigned serial{};
    ImTextureID texture{};
    std::string ready_path;     // the icon the texture holds
};
ModIcon& mod_icon() { static auto* value = new ModIcon; return *value; } // process lifetime

void load_mod_icons() {
    auto& m = mod_icon();
    for (;;) {
        std::string path;
        {
            std::lock_guard lock(m.mutex);
            if (m.wanted == m.tried) {
                m.loading = false;
                return;
            }
            path = m.wanted;
        }
        CardPixels pixels;
        bool ok{};
        try {
            std::ifstream input(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
            const std::string png{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
            ok = !png.empty() && png.size() <= 4 * 1024 * 1024 && decode_picture(png, pixels, 128);
        } catch (...) {}
        if (!ok) pixels = {};
        std::lock_guard lock(m.mutex);
        m.tried = path;
        if (m.decoded.size() >= ModIcon::kept) m.decoded.erase(m.decoded.begin());
        m.decoded.emplace_back(path, pixels);
        if (ok && path == m.wanted) {
            pixels.serial = ++m.serial;
            m.current = std::move(pixels);
            m.current_path = path;
            m.waiting = true;
        }
    }
}
// Asks for this icon (empty: none); the icon's texture once it is on the GPU, else 0.
ImTextureID want_mod_icon(const std::string& path) {
    auto& m = mod_icon();
    std::lock_guard lock(m.mutex);
    if (path.empty()) return {};
    if (m.wanted != path) {
        m.wanted = path;
        // One decoded lately goes straight to the GPU (in the next frame).
        const auto known = std::ranges::find(m.decoded, path, &std::pair<std::string, CardPixels>::first);
        if (known != m.decoded.end()) {
            m.tried = path;
            if (!known->second.rgba.empty() && m.ready_path != path) {
                m.current = known->second;
                m.current.serial = ++m.serial;
                m.current_path = path;
                m.waiting = true;
            }
        } else if (!m.loading) {
            try {
                std::thread(load_mod_icons).detach();
                m.loading = true;
            } catch (...) {}
        }
    }
    return m.ready_path == path ? m.texture : ImTextureID{};
}

// A five-pointed star, the mark favorites carry on their tiles.
void star(ImDrawList* draw, ImVec2 centre, float radius, ImU32 colour) {
    std::array<ImVec2, 10> points;
    for (int i = 0; i < 10; ++i) {
        const float angle = -1.5707963f + static_cast<float>(i) * 0.6283185f, r = i % 2 ? radius * 0.42f : radius;
        points[static_cast<std::size_t>(i)] = ImVec2(centre.x + std::cos(angle) * r, centre.y + std::sin(angle) * r);
    }
    // Concave: fan it from the centre.
    for (int i = 0; i < 10; ++i)
        draw->AddTriangleFilled(centre, points[static_cast<std::size_t>(i)], points[static_cast<std::size_t>((i + 1) % 10)], colour);
}
// Where the game's menu is across the screen: its left edge and width. The full width up to 21:9;
// on a wider screen a 21:9 frame in the middle.
std::pair<float, float> menu_frame(ImVec2 display) {
    const float width = std::min(display.x, display.y * 21.0f / 9.0f);
    return {(display.x - width) * 0.5f, width};
}

// `text` cut short with "..." to fit `width`.
std::string fitted(ImFont* font, float size, const std::string& text, float width) {
    if (font->CalcTextSizeA(size, FLT_MAX, 0, text.c_str()).x <= width) return text;
    std::string cut = text;
    while (!cut.empty()) {
        do cut.pop_back(); while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xc0) == 0x80);
        while (!cut.empty() && cut.back() == ' ') cut.pop_back();
        if (font->CalcTextSizeA(size, FLT_MAX, 0, (cut + "...").c_str()).x <= width) break;
    }
    return cut + "...";
}

// Which mod the highlighted item is from, inside the game's own item card (the rarity tag, the
// name and the description at the bottom of the screen): the mod's icon, what it is to the item,
// its name, who made it, its version and how many items it has. The card grows upward from a
// fixed bottom edge as its text wraps; a mod's item has three empty description lines
// (Customization/local_cosmetic_catalog.cpp), so its last three lines are always the same place.
// As measured on 1280x720 and on 2000 wide at nine heights: the game lays its menu out across
// the screen's width up to 21:9, centred past that (menu_frame); it sizes it by the smaller of its
// two scales (`u`); the prompt row's bottom margin goes with the screen's height, the item card
// sits on the prompt row, its left edge is 45.25% of the frame and it is 319 wide.
void draw_mod_card(ImDrawList* draw, const ItemBrowserView& view, ImFont* heading, ImFont* body) {
    if (view.mod_title.empty()) return;
    const auto display = ImGui::GetIO().DisplaySize;
    const auto [left, width] = menu_frame(display);
    const float u = std::min(width / 1280.0f, display.y / 720.0f);
    const float prompts_top = display.y * (1.0f - 35.0f / 720.0f) - 37 * u;   // Hide Outfit, Equip, Back
    const float card_bottom = prompts_top - 6 * u;                             // the game's item card
    const float card_left = left + width * 0.4525f, card_width = 319 * u;
    // Its last three lines (20 each, over an 8 bottom margin), inset like its text. The first line
    // keeps the info sign the game's notice shows there (item_browser.cpp, quiet_notices); this
    // goes under it, over the other two.
    const float pad = 6 * u, icon = 40 * u;
    const ImVec2 a(card_left + 12 * u - pad, card_bottom - 68 * u), b(card_left + card_width - 12 * u, card_bottom - 8 * u);
    const float top = a.y + 20 * u;
    // The icon, or the title's first letter on a plain tile when the mod has none.
    const ImVec2 i0(a.x + pad, top), i1(i0.x + icon, i0.y + icon);
    if (const auto texture = want_mod_icon(view.mod_icon)) {
        draw->AddImage(texture, i0, i1);
    } else {
        draw->AddRectFilled(i0, i1, skate_theme::tile_light);
        const char letter[2]{static_cast<char>(std::toupper(static_cast<unsigned char>(view.mod_title.front()))), 0};
        const float size = 24 * u;
        const auto extent = heading->CalcTextSizeA(size, FLT_MAX, 0, letter);
        draw->AddText(heading, size, ImVec2(i0.x + (icon - extent.x) * 0.5f, i0.y + (icon - extent.y) * 0.5f), theme::muted, letter);
    }
    const float text_x = i1.x + 10 * u, right = b.x - pad;
    // The tag, like the game's rarity tag, then the name beside it.
    const char* tag = view.mod_changed ? "CHANGED BY MOD" : "MOD";
    const float tag_size = 10 * u, title_size = 15 * u, line_size = 12 * u;
    const auto tag_extent = heading->CalcTextSizeA(tag_size, FLT_MAX, 0, tag);
    const float row = top + (icon - title_size - 5 * u - line_size) * 0.5f;
    const ImVec2 t0(text_x, row + (title_size - tag_extent.y - 4 * u) * 0.5f);
    const ImVec2 t1(t0.x + tag_extent.x + 10 * u, t0.y + tag_extent.y + 4 * u);
    draw->AddRectFilled(t0, t1, view.mod_changed ? skate_theme::bar : skate_theme::blue);
    draw->AddText(heading, tag_size, ImVec2(t0.x + 5 * u, t0.y + 2 * u), view.mod_changed ? skate_theme::black : theme::paper, tag);
    const float name_x = t1.x + 7 * u;
    draw->AddText(heading, title_size, ImVec2(name_x, row), theme::paper,
                  fitted(heading, title_size, view.mod_title, right - name_x).c_str());
    std::string line = view.mod_author.empty() ? std::string() : "by " + view.mod_author;
    const auto add = [&](const std::string& part) { if (!part.empty()) line += (line.empty() ? "" : "  |  ") + part; };
    if (!view.mod_version.empty()) add("v" + view.mod_version);
    if (view.mod_items) add(std::to_string(view.mod_items) + (view.mod_items == 1 ? " item" : " items"));
    draw->AddText(body, line_size, ImVec2(text_x, row + title_size + 5 * u), theme::muted,
                  fitted(body, line_size, line, right - text_x).c_str());
}


std::string upper(std::string text) {
    for (auto& c : text) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - ('a' - 'A'));
    return text;
}

// The type tabs: a bar of their own between the slot icons and the grid, where the game writes the
// open slot's name (the slot's icon shows it already). It stays put while the grid scrolls. Laid out
// for the game's 1080-line screen from its left edge, as the grid is; the tabs scroll sideways to
// keep the picked one in view.
void draw_tabs(ImDrawList* draw, Panel& p, ImFont* bold, ImFont* body, ImVec2 display) {
    auto& bar = p.bar;
    const auto& view = p.view;
    bar.shown = false;
    if (view.tabs.size() < 3) return;
    const auto now = Clock::now();
    const float dt = std::clamp(std::chrono::duration<float>(now - p.drawn).count(), 0.0f, 0.1f);
    p.drawn = now;
    const float u = (std::min)(display.y / 1080.0f, display.x / 1920.0f);
    bar.display = display;
    bar.a = ImVec2(107.5f * u, 257.0f * u);
    bar.b = ImVec2(849.5f * u, 305.0f * u);
    const float middle = (bar.a.y + bar.b.y) * 0.5f;
    draw->AddRectFilled(bar.a, bar.b, IM_COL32(22, 26, 30, 250));
    draw->AddLine(ImVec2(bar.a.x, bar.b.y - u), ImVec2(bar.b.x, bar.b.y - u), IM_COL32(255, 255, 255, 18), u);

    // The prompts at both ends, like the game's own keycaps.
    const bool pad = p.pad_prompts.load();
    const char* left_key = pad ? "LT" : "Z";
    const char* right_key = pad ? "RT" : "C";
    const float cap_size = 15 * u, cap_height = 24 * u, edge = 9 * u;
    const auto cap = [&](float x, const char* key) {
        const auto extent = bold->CalcTextSizeA(cap_size, FLT_MAX, 0, key);
        const float width = (std::max)(extent.x + 14 * u, cap_height);
        const ImVec2 a(x, middle - cap_height * 0.5f), b(x + width, middle + cap_height * 0.5f);
        const bool hovered = p.mouse.x >= a.x - 4 * u && p.mouse.x <= b.x + 4 * u && p.mouse.y >= bar.a.y && p.mouse.y <= bar.b.y;
        draw->AddRectFilled(a, b, hovered ? IM_COL32(255, 255, 255, 255) : theme::paper, 3 * u);
        draw->AddText(bold, cap_size, ImVec2(a.x + (width - extent.x) * 0.5f, middle - extent.y * 0.5f), skate_theme::black, key);
        return width;
    };
    const float left_width = cap(bar.a.x + edge, left_key);
    bar.previous_x0 = bar.a.x;
    bar.previous_x1 = bar.a.x + edge + left_width + 4 * u;
    const auto right_extent = bold->CalcTextSizeA(cap_size, FLT_MAX, 0, right_key);
    const float right_width = (std::max)(right_extent.x + 14 * u, cap_height);
    cap(bar.b.x - edge - right_width, right_key);
    bar.next_x0 = bar.b.x - edge - right_width - 4 * u;
    bar.next_x1 = bar.b.x;
    bar.clip_x0 = bar.a.x + edge + left_width + 10 * u;
    bar.clip_x1 = bar.b.x - edge - right_width - 10 * u;

    // The tabs: name and count, the picked one filled with the game's blue.
    const float name_size = 17 * u, count_size = 14 * u, pad_x = 12 * u, gap = 3 * u, chip_height = 34 * u;
    struct Chip { std::string name, count; float name_width{}, width{}; };
    std::vector<Chip> chips;
    float total = 0;
    for (const auto& tab : view.tabs) {
        Chip chip{upper(tab.name), std::to_string(tab.shown)};
        chip.name_width = bold->CalcTextSizeA(name_size, FLT_MAX, 0, chip.name.c_str()).x;
        chip.width = pad_x * 2 + chip.name_width + 7 * u + body->CalcTextSizeA(count_size, FLT_MAX, 0, chip.count.c_str()).x;
        total += chip.width + (chips.empty() ? 0 : gap);
        chips.push_back(std::move(chip));
    }
    const float room = bar.clip_x1 - bar.clip_x0;
    const float most = (std::max)(0.0f, total - room);
    // Scroll so the picked tab and a bit of its neighbours show.
    float start = 0;
    for (std::size_t i = 0; i < chips.size() && i < view.tab; ++i) start += chips[i].width + gap;
    const float end = start + (view.tab < chips.size() ? chips[view.tab].width : 0);
    float target = p.scroll;
    if (target < end + 28 * u - room) target = end + 28 * u - room;
    if (target > start - 28 * u) target = start - 28 * u;
    target = std::clamp(target, 0.0f, most);
    p.scroll += (target - p.scroll) * (std::min)(1.0f, dt * 14.0f);
    if (std::abs(target - p.scroll) < 0.5f) p.scroll = target;

    bar.tabs.clear();
    draw->PushClipRect(ImVec2(bar.clip_x0, bar.a.y), ImVec2(bar.clip_x1, bar.b.y), true);
    float x = bar.clip_x0 - p.scroll;
    for (std::size_t i = 0; i < chips.size(); ++i) {
        const auto& chip = chips[i];
        const auto& tab = view.tabs[i];
        const ImVec2 a(x, middle - chip_height * 0.5f), b(x + chip.width, middle + chip_height * 0.5f);
        bar.tabs.emplace_back((std::max)(a.x, bar.clip_x0), (std::min)(b.x, bar.clip_x1));
        const bool picked = i == view.tab, empty = tab.shown == 0 && i != 0;
        const bool hovered = !empty && p.mouse.x >= a.x && p.mouse.x <= b.x && p.mouse.x >= bar.clip_x0 &&
                             p.mouse.x <= bar.clip_x1 && p.mouse.y >= bar.a.y && p.mouse.y <= bar.b.y;
        if (picked) draw->AddRectFilled(a, b, theme::blue);
        else if (hovered) draw->AddRectFilled(a, b, IM_COL32(45, 45, 47, 255));
        const ImU32 name_colour = picked ? theme::paper : empty ? IM_COL32(92, 94, 98, 255) : IM_COL32(214, 214, 216, 255);
        const ImU32 count_colour = picked ? IM_COL32(255, 255, 255, 190) : empty ? IM_COL32(78, 80, 84, 255) : theme::muted;
        const float name_y = middle - name_size * 0.5f - u;
        draw->AddText(bold, name_size, ImVec2(a.x + pad_x, name_y), name_colour, chip.name.c_str());
        draw->AddText(body, count_size, ImVec2(a.x + pad_x + chip.name_width + 7 * u, middle - count_size * 0.5f),
                      count_colour, chip.count.c_str());
        x += chip.width + gap;
    }
    draw->PopClipRect();
    // Fades at an edge with more tabs beyond it.
    const float fade = 30 * u;
    const ImU32 solid = IM_COL32(22, 26, 30, 250), clear = IM_COL32(22, 26, 30, 0);
    if (p.scroll > 0.5f)
        draw->AddRectFilledMultiColor(ImVec2(bar.clip_x0, bar.a.y + u), ImVec2(bar.clip_x0 + fade, bar.b.y - u), solid, clear, clear, solid);
    if (p.scroll < most - 0.5f)
        draw->AddRectFilledMultiColor(ImVec2(bar.clip_x1 - fade, bar.a.y + u), ImVec2(bar.clip_x1, bar.b.y - u), clear, solid, solid, clear);
    bar.shown = true;
}
} // namespace

bool take_mod_icon_pixels(CardPixels& out) {
    auto& m = mod_icon();
    std::lock_guard lock(m.mutex);
    if (!m.waiting) return false;
    m.waiting = false;
    out = m.current;
    return true;
}
void mod_icon_uploaded(ImTextureID texture, unsigned serial) {
    auto& m = mod_icon();
    std::lock_guard lock(m.mutex);
    if (serial != m.serial) return;
    m.texture = texture;
    m.ready_path = m.current_path;
}
void mod_icon_lost() {
    auto& m = mod_icon();
    std::lock_guard lock(m.mutex);
    m.texture = {};
    m.ready_path.clear();
    if (!m.current.rgba.empty()) m.waiting = true;
}

namespace acc = dingosdk::accessory_probe;
void fit_capture(FitPanel& f, bool hold) {
    if (hold == f.capture) return;
    if (hold) keep_xinput_capture_first(); // other programs' XInput hooks (Steam's overlay) must not read past it
    state().item_panel_active.store(hold, std::memory_order_relaxed);
    f.capture = hold;
}
// The keys the panel answers to, read from the desktop: while it holds the input the game's key messages are
// swallowed before they reach the overlay's own key state.
bool panel_key(int vk) {
    const HWND window = state().window.load();
    if (!window || !game_window_foreground(window)) return false;
    if (std::array<std::uint64_t, 4> held{}; sampled_keys(window, held)) return ((held[static_cast<std::size_t>(vk) / 64] >> (vk % 64)) & 1) != 0;
    OverlayInputAccess access;
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}
void fit_close(FitPanel& f) {
    if (f.open) f.release = true;
    f.open = false;
}
// Once a presented frame, while a grid is open.
void update_fit(Panel& p, const dingosdk::ControllerInput& pad) {
    namespace ib = dingosdk::item_browser;
    auto& f = p.fit;
    const auto pick = acc::model_pick(p.view.focused_key);
    const bool ours = !pick.model.empty();
    const int place = ours ? acc::fit_place(pick.kind, pick.place) : -1;
    const std::uint32_t bits = pad.available ? pad.buttons : 0u;
    const auto key = [](int vk) { return panel_key(vk); };
    const std::array<bool, 8> k{key(VK_UP), key(VK_DOWN), key(VK_LEFT), key(VK_RIGHT), key(VK_RETURN),
                                key(VK_ESCAPE) || key(VK_BACK), key('V'), key(VK_SPACE)};
    const auto pressed = [&](std::uint32_t b) { return (bits & b) != 0 && (f.buttons & b) == 0; };
    const auto key_pressed = [&](std::size_t i) { return k[i] && !f.keys[i]; };
    // R3 is ours on our keychains (the game would show / hide the outfit with it).
    if (ours || f.open) hold_item_browser_buttons(XINPUT_GAMEPAD_RIGHT_THUMB);
    if (place >= 0) acc::prepare_fit();
    if (ours) acc::set_shown_item(p.view.focused_key);   // fit57: the DLL serves the keychain the skater shows
    const bool toggle = pressed(XINPUT_GAMEPAD_RIGHT_THUMB) || key_pressed(6);
    // fit26: equip the picked place's item. The game equips the item a tile was FOCUSED with (fit19) and does not take
    // a tile pointed at another item (fit24), so the tile becomes the picked item (item_browser arrange) and the focus
    // goes off it and back, then A. A tap is short and the next waits for its release (fit25: a back tap while the
    // away tap was still held was ignored, the focus stayed on the neighbour); the back step repeats toward home.
    if (!f.equip.empty()) {
        std::string focused(p.view.focused_shows), worn = acc::fit_view().outfit;
        for (auto* text : {&focused, &worn})
            for (auto& c : *text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        const auto now = Clock::now();
        const auto since = now - f.step_at;
        const auto ms = [&](Clock::time_point t) {
            return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(t - f.equip_at).count());
        };
        const int index = p.view.focused_index;
        if (index != f.last_index) f.trace += std::format(" | {} focus {}", ms(now), index), f.last_index = index;
        constexpr auto tap_ms = 60u;
        const bool released = since > std::chrono::milliseconds(tap_ms + 45); // the last tap is over, a poll without it
        // away: left / right first (up from the first row would leave the grid), then up / down
        constexpr std::array<std::uint16_t, 4> away{XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT,
                                                    XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN};
        constexpr int columns = 4;
        // the d-pad step from `from` toward `to` in the grid
        const auto toward = [](int from, int to) -> std::uint16_t {
            if (to == from - 1) return XINPUT_GAMEPAD_DPAD_LEFT;
            if (to == from + 1) return XINPUT_GAMEPAD_DPAD_RIGHT;
            if (to < from) return XINPUT_GAMEPAD_DPAD_UP;
            return XINPUT_GAMEPAD_DPAD_DOWN;
        };
        const auto tap = [&](std::uint16_t bits, const char* what) {
            tap_item_browser_buttons(bits, tap_ms), f.step_at = now;
            f.trace += std::format(" | {} {}", ms(now), what);
        };
        const auto finish = [&](const char* how) {
            static unsigned lines = 80;
            if (lines && lines--)
                logging::printf(logging::Level::info, logging::Channel::customization,
                    "Accessories: equip %s: %s after %lld ms (stage %d, home %d, worn %s)%s", f.equip.c_str(), how, ms(now),
                    f.stage, f.home, worn.c_str(), f.trace.c_str());
            f.equip.clear(), f.stage = 0, f.trace.clear(), f.last_index = -1;
        };
        if (now > f.equip_until) finish("given up (time)");
        else if (f.stage == 0) {
            if (focused == f.equip && index > 0) {
                if (worn == f.equip) finish("already worn");
                else {
                    f.home = index, f.tries = 0;
                    // a tile in the first column goes right first (left of it is the row above's end)
                    f.dir = f.home % columns == 0 && f.home + 1 < static_cast<int>(p.view.rows) ? 1 : 0;
                    tap(away[static_cast<std::size_t>(f.dir)], "away"), f.stage = 1;
                }
            }
        } else if (f.stage == 1) {
            if (index >= 0 && index != f.home) {
                if (released) tap(toward(index, f.home), "back"), f.stage = 2, f.tries = 0, f.arrived = {};
            } else if (since > std::chrono::milliseconds(450)) {
                if (++f.tries >= 4) finish("given up (the focus does not move)");
                else f.dir = (f.dir + 1) % 4, tap(away[static_cast<std::size_t>(f.dir)], "away again");
            }
        } else if (f.stage == 2) {
            if (index == f.home && focused == f.equip) {
                if (f.arrived == Clock::time_point{}) f.arrived = now;
                // the game's focus script names the item a moment after the focus arrives (fit20 waited 220 ms)
                if (released && now - f.arrived > std::chrono::milliseconds(200)) tap(XINPUT_GAMEPAD_A, "A"), f.stage = 3;
            } else if (index >= 0 && index != f.home && since > std::chrono::milliseconds(350)) {
                if (++f.tries >= 4) finish("given up (the focus does not come back, no A)");
                else tap(toward(index, f.home), "back again");
            } else if (since > std::chrono::milliseconds(900)) {
                finish("given up (the tile does not show the pick, no A)");
            }
        } else if (f.stage == 3) {
            if (worn == f.equip) finish("worn");
            else if (since > std::chrono::milliseconds(1500)) finish("not seen worn yet");
        }
    }
    const bool nudging = !f.equip.empty() && f.stage > 0 && f.stage < 4;
    const auto now_t = Clock::now();
    if (ours) f.lost_since = {};
    else if (f.open && f.lost_since == Clock::time_point{}) f.lost_since = now_t;
    const bool lost = !ours && f.open && now_t - f.lost_since > std::chrono::milliseconds(1500);
    if (!f.open) {
        if (toggle && ours) f.open = true, f.row = 0, f.place = place, f.model = pick.model, f.kind = pick.kind, f.repeat = 0;
    } else if (toggle || pressed(XINPUT_GAMEPAD_B) || key_pressed(5) || (ours && pick.model != f.model && !nudging) || lost) {
        fit_close(f);
    } else if (!ours || nudging) {
        // the game is rebuilding its list (after an equip), or the panel moves its focus: the panel waits
    } else {
        f.place = place; // follows the place picked
        const auto list = fit_row_list(f.kind, pick);
        const int fit_rows = static_cast<int>(list.size());
        f.row = std::clamp(f.row, 0, fit_rows - 1);
        if (pressed(XINPUT_GAMEPAD_DPAD_UP) || key_pressed(0)) f.row = (f.row + fit_rows - 1) % fit_rows;
        if (pressed(XINPUT_GAMEPAD_DPAD_DOWN) || key_pressed(1)) f.row = (f.row + 1) % fit_rows;
        const auto row = list[static_cast<std::size_t>(f.row)];
        // Left / right change the row's value: once, then again and again while held.
        const bool left = (bits & XINPUT_GAMEPAD_DPAD_LEFT) != 0 || k[2], right = (bits & XINPUT_GAMEPAD_DPAD_RIGHT) != 0 || k[3];
        const int dir = left == right ? 0 : left ? -1 : 1;
        const auto now = Clock::now();
        int step = 0;
        if (dir != f.repeat) {
            step = dir;
            f.repeat_at = now + std::chrono::milliseconds(350);
        } else if (dir && now >= f.repeat_at) {
            step = dir;
            f.repeat_at = now + std::chrono::milliseconds(70);
        }
        f.repeat = dir;
        const bool press = pressed(XINPUT_GAMEPAD_A) || key_pressed(4) || key_pressed(7);
        auto o = acc::place_offset(f.place);
        const auto nudge = [&](float& value, float low, float high) {   // fit51: 0.25 cm steps (finer)
            value = std::clamp(std::round((value + 0.25f * static_cast<float>(step)) * 4.0f) / 4.0f, low, high);
        };
        const auto turn = [&](float& value, float low, float high) {   // fit51: 1 degree steps
            value = std::clamp(std::round(value + 1.0f * static_cast<float>(step)), low, high);
        };
        const auto kind = static_cast<ib::AccessoryKind>(f.kind);
        // fit28: the item for a place, charm set and sound - the nearest installed one (the tile shows the same)
        const auto item_for = [&](std::string_view charm, bool quiet, std::string_view to) {
            for (const auto& key : {ib::variant_key(f.model, charm, quiet, to), ib::variant_key(f.model, "", quiet, to),
                                    ib::variant_key(f.model, charm, false, to)})
                if (acc::installed(key)) return key;
            return ib::variant_key(f.model, "", false, to);
        };
        const auto equip = [&](std::string key) {
            f.equip = std::move(key);
            f.stage = 0, f.trace.clear(), f.last_index = -1;
            f.equip_at = Clock::now();
            f.equip_until = f.equip_at + std::chrono::milliseconds(4000);
        };
        const auto choose = [&](std::string_view to) {
            acc::pick_place(f.model, to);
            equip(item_for(pick.charm, pick.quiet, to));
        };
        if ((step || press) && row == FitRow::charm && pick.charms.size() > 1) {
            const auto at = std::find(pick.charms.begin(), pick.charms.end(), pick.charm);
            const int i = at == pick.charms.end() ? 0 : static_cast<int>(at - pick.charms.begin());
            const int n = static_cast<int>(pick.charms.size());
            const auto& set = pick.charms[static_cast<std::size_t>((i + (step ? step : 1) + n) % n)];
            // a set not made for the place picked: the first place of the strap group that has it, else any
            const auto place_with = [&]() -> std::string {
                if (acc::installed(ib::variant_key(f.model, set, false, pick.place))) return pick.place;
                for (const auto p : ib::place_group(kind, pick.place))
                    if (acc::installed(ib::variant_key(f.model, set, false, p))) return std::string(p);
                for (const auto p : ib::keychain_places)
                    if (acc::installed(ib::variant_key(f.model, set, false, p))) return std::string(p);
                return pick.place;
            };
            const auto to = place_with();
            if (to != pick.place) acc::pick_place(f.model, to);
            acc::pick_charm(f.model, set);
            equip(item_for(set, pick.quiet, to));
        }
        if ((step || press) && row == FitRow::sound) {
            acc::pick_sound(pick.quiet);   // quiet -> sound on, and back
            equip(item_for(pick.charm, !pick.quiet, pick.place));
        }
        // fit47: only to places the model has an item for (the charm slots test is at the back only: the row went to
        // places without it and Up / Turn then moved those places, not the item worn)
        const auto has = [&](std::string_view at_place) {
            return pick.places.empty() || std::find(pick.places.begin(), pick.places.end(), at_place) != pick.places.end();
        };
        if ((step || press) && row == FitRow::strap && has(ib::strap_toggled(pick.place))) choose(ib::strap_toggled(pick.place));
        if ((step || press) && row == FitRow::place) {
            std::vector<std::string_view> group;
            for (const auto in_group : ib::place_group(kind, pick.place))
                if (has(in_group)) group.push_back(in_group);
            if (group.empty()) group.push_back(pick.place);
            const auto at = std::find(group.begin(), group.end(), std::string_view(pick.place));
            const int i = at == group.end() ? 0 : static_cast<int>(at - group.begin());
            const int n = static_cast<int>(group.size());
            choose(group[static_cast<std::size_t>((i + (step ? step : 1) + n) % n)]);
        }
        if (f.place >= 0) {
            if (step && row == FitRow::up) nudge(o.up, -15.0f, 15.0f), acc::set_offset(f.place, o);
            if (step && row == FitRow::out) nudge(o.out, -10.0f, 10.0f), acc::set_offset(f.place, o);
            if (step && row == FitRow::along) nudge(o.along, -10.0f, 10.0f), acc::set_offset(f.place, o);
            if (step && row == FitRow::turn) turn(o.turn, -60.0f, 60.0f), acc::set_offset(f.place, o);
            if (step && row == FitRow::tilt) turn(o.tilt, -45.0f, 45.0f), acc::set_offset(f.place, o);
            if ((step || press) && row == FitRow::pants && acc::pants_worn()) acc::set_pants_own(!acc::pants_own());
            if ((step || press) && row == FitRow::finish) {
                const int n = acc::finish_count();
                acc::set_finish((acc::current_finish() + (step ? step : 1) + n) % n);
            }
            if ((step || press) && row == FitRow::strapcolor) {
                const int n = acc::strap_count();   // fit58: the strap's own palette
                acc::set_strap((acc::current_strap() + (step ? step : 1) + n) % n);
            }
            if ((step || press) && (row == FitRow::charm1 || row == FitRow::charm2 || row == FitRow::charm3)) {   // fit36
                const int slot = static_cast<int>(row) - static_cast<int>(FitRow::charm1);
                acc::step_charm(slot, step ? step : 1);   // fit59: None, then every option not hung in another slot
            }
            if (press && row == FitRow::reset) acc::set_offset(f.place, {});
        }
    }
    const bool any = bits != 0 || std::any_of(k.begin(), k.end(), [](bool b) { return b; });
    if (!any) f.release = false;
    fit_capture(f, f.open || f.release);
    if (f.open && f.place >= 0) acc::set_focus(f.place, p.aspect);
    {   // fit52-54 (user: "the camera should always turn to the carabiner"; on fit53: "switching places fast it does not
        // keep up, front -> back often does not turn - make it much steadier"). While the panel is open the keychain
        // is HELD towards the camera: whenever its side is more than 20 deg off (a place change, the equip that follows
        // it ~0.5-0.9 s later, anything), the wardrobe skater is turned with the right stick until within 5 deg.
        //  - the stick's direction is measured (the error's rate while it is held 0.3 s) and kept (profile);
        //  - jumps of the error (a snap of the skater, > 0.25 rad in one frame) are not taken as a turn;
        //  - the way round is chosen when a turn starts and kept while more than 90 deg are left;
        //  - braking ahead: stops when what is left is less than 0.3 s of the measured speed;
        //  - the player's own right stick: the panel leaves the turning to him until the next place change;
        //  - a turn that has not got there in 6 s stops (and waits 2 s).
        static int seen_place = -2;
        static bool turning{}, player{};
        static std::uint64_t place_at{}, last_at{}, held_since{}, turn_at{}, rest_until{};
        static float dir{}, last_err{}, rate{}, held_stick{};
        const auto now = GetTickCount64();
        if (!(f.open && f.place >= 0)) {
            if (seen_place != -2) turn_item_browser_skater(0.0f, 0), turning = false;
            seen_place = -2;
        } else {
            float err{};
            const bool fresh = acc::panel_turn_error(err);
            if (f.place != seen_place) seen_place = f.place, place_at = now, player = false, rest_until = 0;
            if (item_browser_user_turned_at() > place_at + 100) {
                if (!player) acc::panel_note("panel turn: the player turns the skater himself - left to him until the next place change");
                player = true;
            }
            if (fresh) {
                float de = err - last_err;
                if (de > 3.14159265f) de -= 6.2831853f;
                if (de < -3.14159265f) de += 6.2831853f;
                if (last_at && now > last_at && now - last_at < 200 && std::fabs(de) < 0.25f)
                    rate += (de * 1000.0f / static_cast<float>(now - last_at) - rate) * 0.35f;   // rad / s, smoothed
                else if (std::fabs(de) >= 0.25f) rate = 0.0f;                                 // a snap: not a turn
                last_err = err, last_at = now;
            }
            int learnt = acc::panel_turn_sign();
            if (turning && held_stick != 0.0f && now - held_since > 300 && std::fabs(rate) > 0.6f && std::fabs(rate) < 8.0f) {
                const int seen = (rate > 0.0f) == (held_stick > 0.0f) ? 1 : -1;
                if (seen != learnt) acc::set_panel_turn_sign(seen), learnt = seen;
            }
            float stick = 0.0f;
            if (!player && fresh && now >= rest_until) {
                const float a = std::fabs(err);
                if (!turning && a > 0.35f) {
                    turning = true, dir = 0.0f, turn_at = now;
                    acc::panel_note(std::format("panel turn: place {}, {:.0f} deg to go", f.place, a * 57.2958f));
                }
                if (turning) {
                    if (dir == 0.0f || a < 1.5708f) dir = err > 0.0f ? 1.0f : -1.0f;   // the way round, kept beyond 90 deg
                    const bool closing = rate * dir < 0.0f;
                    if (a < 0.087f || (closing && a < std::fabs(rate) * 0.3f)) {
                        turning = false;
                        acc::panel_note(std::format("panel turn: there in {} ms ({:.0f} deg off)", now - turn_at, a * 57.2958f));
                    } else if (now - turn_at > 6000) {
                        turning = false, rest_until = now + 2000;
                        acc::panel_note(std::format("panel turn: not there after 6 s ({:.0f} deg off, rate {:.2f}) - waiting", a * 57.2958f, rate));
                    } else {
                        stick = -static_cast<float>(learnt ? learnt : 1) * dir * (0.45f + 0.55f * std::min(a / 1.2f, 1.0f));
                    }
                }
            } else if (turning && (player || !fresh)) {
                turning = false;
            }
            if ((stick > 0.0f) != (held_stick > 0.0f) || stick == 0.0f || held_stick == 0.0f) held_since = now;
            held_stick = stick;
            turn_item_browser_skater(stick, stick != 0.0f ? 150 : 0);
        }
    }
    f.buttons = bits;
    f.keys = k;
}

std::string item_name(std::string key) {
    for (auto& c : key) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    for (const char* prefix : {"own_keychain_", "own_lanyard_", "own_wallet_", "own_"})
        if (key.rfind(prefix, 0) == 0) { key.erase(0, std::char_traits<char>::length(prefix)); break; }
    bool start = true;
    for (auto& c : key) {
        if (c == '_') { c = ' '; start = true; continue; }
        if (start && c >= 'a' && c <= 'z') c = static_cast<char>(c - ('a' - 'A'));
        start = false;
    }
    return key;
}
// fit31: the panel's row icons, drawn (no textures): about `s` pixels wide around `c`.
void row_icon(ImDrawList* d, FitRow row, ImVec2 c, float s, ImU32 col, ImU32 swatch, bool off) {
    const float t = std::max(1.2f, s * 0.11f), h = s * 0.5f;
    const auto P = [&](float x, float y) { return ImVec2(c.x + x * h, c.y + y * h); };
    const auto arrow = [&](ImVec2 a, ImVec2 b) {            // a line with a head at b
        d->AddLine(a, b, col, t);
        const float dx = b.x - a.x, dy = b.y - a.y, l = std::max(0.001f, std::sqrt(dx * dx + dy * dy));
        const float ux = dx / l, uy = dy / l, k = h * 0.42f;
        d->AddTriangleFilled(b, ImVec2(b.x - ux * k - uy * k * 0.6f, b.y - uy * k + ux * k * 0.6f),
                             ImVec2(b.x - ux * k + uy * k * 0.6f, b.y - uy * k - ux * k * 0.6f), col);
    };
    switch (row) {
    case FitRow::strap:   // a ring with the band hanging from it
        d->AddCircle(P(0, -0.62f), h * 0.3f, col, 14, t);
        d->AddRect(P(-0.3f, -0.3f), P(0.3f, 0.95f), col, h * 0.25f, 0, t);
        d->AddLine(P(-0.3f, 0.12f), P(0.3f, 0.3f), col, t * 0.7f);
        d->AddLine(P(-0.3f, 0.5f), P(0.3f, 0.68f), col, t * 0.7f);
        break;
    case FitRow::charm: case FitRow::charm1: case FitRow::charm2: case FitRow::charm3:   // a key
        d->AddCircle(P(-0.5f, -0.45f), h * 0.38f, col, 16, t);
        d->AddLine(P(-0.25f, -0.2f), P(0.75f, 0.8f), col, t);
        d->AddLine(P(0.35f, 0.4f), P(0.6f, 0.15f), col, t);
        d->AddLine(P(0.55f, 0.6f), P(0.8f, 0.35f), col, t);
        break;
    case FitRow::place:   // a map pin
        d->AddCircle(P(0, -0.3f), h * 0.45f, col, 16, t);
        d->AddLine(P(-0.38f, -0.05f), P(0, 0.85f), col, t);
        d->AddLine(P(0.38f, -0.05f), P(0, 0.85f), col, t);
        d->AddCircleFilled(P(0, -0.3f), h * 0.15f, col, 10);
        break;
    case FitRow::up:
        arrow(P(0, 0.2f), P(0, -0.9f));
        arrow(P(0, -0.2f), P(0, 0.9f));
        break;
    case FitRow::out:     // away from the leg
        d->AddLine(P(-0.8f, -0.9f), P(-0.8f, 0.9f), col, t);
        arrow(P(-0.5f, 0), P(0.9f, 0));
        break;
    case FitRow::along:
        arrow(P(0.2f, 0), P(-0.9f, 0));
        arrow(P(-0.2f, 0), P(0.9f, 0));
        break;
    case FitRow::turn: {  // a turning arrow
        d->PathArcTo(c, h * 0.7f, 0.6f, 5.2f, 20);
        d->PathStroke(col, 0, t);
        const ImVec2 e(c.x + std::cos(5.2f) * h * 0.7f, c.y + std::sin(5.2f) * h * 0.7f);
        d->AddTriangleFilled(e, ImVec2(e.x - h * 0.45f, e.y - h * 0.05f), ImVec2(e.x - h * 0.05f, e.y + h * 0.45f), col);
        break;
    }
    case FitRow::tilt:    // an angle
        d->AddLine(P(-0.8f, 0.7f), P(0.9f, 0.7f), col, t);
        d->AddLine(P(-0.8f, 0.7f), P(0.6f, -0.8f), col, t);
        d->PathArcTo(P(-0.8f, 0.7f), h * 0.85f, -0.82f, 0.0f, 10);
        d->PathStroke(col, 0, t * 0.8f);
        break;
    case FitRow::pants: { // trousers
        const ImVec2 pts[]{P(-0.6f, -0.85f), P(0.6f, -0.85f), P(0.75f, 0.9f), P(0.2f, 0.9f), P(0, -0.2f), P(-0.2f, 0.9f), P(-0.75f, 0.9f)};
        d->AddPolyline(pts, 7, col, ImDrawFlags_Closed, t);
        break;
    }
    case FitRow::finish:  // the colour picked
        d->AddCircleFilled(c, h * 0.8f, swatch, 20);
        d->AddCircle(c, h * 0.8f, col, 20, t * 0.8f);
        break;
    case FitRow::strapcolor:  // the strap, in its colour
        d->AddRectFilled(P(-0.34f, -0.9f), P(0.34f, 0.9f), swatch, h * 0.3f);
        d->AddRect(P(-0.34f, -0.9f), P(0.34f, 0.9f), col, h * 0.3f, 0, t * 0.8f);
        d->AddLine(P(-0.34f, -0.25f), P(0.34f, -0.05f), col, t * 0.6f);
        d->AddLine(P(-0.34f, 0.3f), P(0.34f, 0.5f), col, t * 0.6f);
        break;
    case FitRow::sound: { // a speaker, crossed when off
        const ImVec2 body[]{P(-0.9f, -0.3f), P(-0.45f, -0.3f), P(0.05f, -0.8f), P(0.05f, 0.8f), P(-0.45f, 0.3f), P(-0.9f, 0.3f)};
        d->AddPolyline(body, 6, col, ImDrawFlags_Closed, t);
        if (off) {
            d->AddLine(P(0.35f, -0.35f), P(0.95f, 0.35f), col, t);
            d->AddLine(P(0.35f, 0.35f), P(0.95f, -0.35f), col, t);
        } else {
            d->PathArcTo(P(0.05f, 0), h * 0.45f, -0.8f, 0.8f, 8);
            d->PathStroke(col, 0, t);
            d->PathArcTo(P(0.05f, 0), h * 0.8f, -0.8f, 0.8f, 10);
            d->PathStroke(col, 0, t);
        }
        break;
    }
    case FitRow::reset: { // back: a turning arrow the other way
        d->PathArcTo(c, h * 0.7f, 3.8f, 8.6f, 20);
        d->PathStroke(col, 0, t);
        const ImVec2 e(c.x + std::cos(3.8f) * h * 0.7f, c.y + std::sin(3.8f) * h * 0.7f);
        d->AddTriangleFilled(e, ImVec2(e.x + h * 0.45f, e.y - h * 0.05f), ImVec2(e.x + h * 0.05f, e.y - h * 0.45f), col);
        break;
    }
    }
}
// The panel: under the search field, in the style of the game's item card.
void draw_fit(ImDrawList* draw, Panel& p, ImFont* heading, ImFont* body, float scale, ImVec2 at, float width) {
    namespace ib = dingosdk::item_browser;
    const auto& f = p.fit;
    const auto pick = f.model.empty() ? acc::model_pick(p.view.focused_key)
                                      : acc::model_pick(f.model + std::string(ib::default_place(static_cast<ib::AccessoryKind>(f.kind))));
    const auto list = fit_row_list(f.kind, pick);
    const int fit_rows = static_cast<int>(list.size());
    const float row_h = 32 * scale, pad = 14 * scale, title = 30 * scale, foot = 36 * scale;
    int sections = 0;
    for (int i = 0, last = -1; i < fit_rows; ++i)
        if (const int sct = fit_section(list[static_cast<std::size_t>(i)]); sct != last) ++sections, last = sct;
    const ImVec2 a = at, b(at.x + width, at.y + pad + title + 20 * scale + row_h * fit_rows + 22 * scale * sections + foot);
    skate_theme::rough_rect(draw, a, b, IM_COL32(18, 18, 19, 240), 83, scale);
    draw->AddRectFilled(a, ImVec2(a.x + 4 * scale, b.y), theme::blue);
    const char* what = f.kind == static_cast<int>(ib::AccessoryKind::wallet) ? "WALLET CHAIN - "
                       : f.kind == static_cast<int>(ib::AccessoryKind::lanyard) ? "LANYARD - " : "KEYCHAIN - ";
    std::string where(ib::place_title(pick.place));
    for (auto& c : where) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - ('a' - 'A'));
    draw->AddText(heading, 18 * scale, ImVec2(a.x + pad, a.y + pad), theme::paper, (std::string(what) + where).c_str());
    draw->AddText(body, 13 * scale, ImVec2(a.x + pad, a.y + pad + 22 * scale), theme::muted, item_name(p.view.focused_key).c_str());
    const auto o = acc::place_offset(f.place);
    const bool side = f.place == 2 || f.place == 5 || f.place == 10 || f.place == 13;
    const bool pants = acc::pants_worn();
    // fit32: what each row shows on its right
    enum class Shown { text, number, toggle, swatch, action };
    struct Row { std::string label, value; Shown shown{Shown::text}; float v{}, lo{}, hi{}; bool on{}; ImU32 sw{}; };
    std::vector<Row> rows;
    for (const auto row : list) {
        switch (row) {
        case FitRow::strap: rows.push_back({"Strap", ib::clip_place(pick.place) ? "Clip only" : "With strap"}); break;
        case FitRow::charm: rows.push_back({"Charms", std::string(ib::charm_title(pick.charm, pick.model))}); break;
        case FitRow::charm1: case FitRow::charm2: case FitRow::charm3: {   // fit36
            const int slot = static_cast<int>(row) - static_cast<int>(FitRow::charm1);
            rows.push_back({std::format("Charm {}", slot + 1), acc::charm_option_name(acc::current_charm(slot))});
            break;
        }
        case FitRow::finish:
            rows.push_back({acc::strap_available() ? "Carabiner color" : "Color", acc::finish_name(acc::current_finish()), Shown::swatch});
            break;
        case FitRow::strapcolor: rows.push_back({"Strap color", acc::strap_name(acc::current_strap()), Shown::swatch}); break;
        case FitRow::sound: rows.push_back({"Sound", "", Shown::toggle, 0, 0, 0, !pick.quiet}); break;
        case FitRow::place: rows.push_back({"Place", std::string(ib::place_title(pick.place))}); break;
        case FitRow::up: rows.push_back({"Up / down", std::format("{:+.2f} cm", o.up), Shown::number, o.up, -15, 15}); break;
        case FitRow::out: rows.push_back({"Off the leg", std::format("{:+.2f} cm", o.out), Shown::number, o.out, -10, 10}); break;
        case FitRow::along: rows.push_back({side ? "Forward / back" : "To the side", std::format("{:+.2f} cm", o.along), Shown::number, o.along, -10, 10}); break;
        case FitRow::turn: rows.push_back({"Turn", std::format("{:+.0f} deg", o.turn), Shown::number, o.turn, -60, 60}); break;
        case FitRow::tilt: rows.push_back({"Tilt", std::format("{:+.0f} deg", o.tilt), Shown::number, o.tilt, -45, 45}); break;
        case FitRow::pants:
            if (pants) rows.push_back({"Only for these pants", "", Shown::toggle, 0, 0, 0, acc::pants_own()});
            else rows.push_back({"Only for these pants", "No pants worn"});
            break;
        case FitRow::reset: rows.push_back({"Back to the mod's place", "", Shown::action}); break;
        }
    }
    const auto to_col = [](const std::array<float, 3>& c) {
        return IM_COL32(static_cast<int>(std::clamp(c[0], 0.0f, 1.0f) * 255), static_cast<int>(std::clamp(c[1], 0.0f, 1.0f) * 255),
                        static_cast<int>(std::clamp(c[2], 0.0f, 1.0f) * 255), 255);
    };
    const auto swatch = to_col(acc::finish_swatch(acc::current_finish()));
    const auto strap_swatch = to_col(acc::strap_swatch(acc::current_strap()));   // fit34; fit58: its own palette
    for (std::size_t i = 0; i < rows.size() && i < list.size(); ++i) rows[i].sw = list[i] == FitRow::strapcolor ? strap_swatch : swatch;
    constexpr const char* section_names[]{"LOOK", "POSITION", "FIT"};
    const float section_h = 22 * scale;
    float y = a.y + pad + title + 20 * scale;
    int section = -1;
    for (int i = 0; i < fit_rows; ++i) {
        const auto row = list[static_cast<std::size_t>(i)];
        if (fit_section(row) != section) {   // a section header: small caps and a hairline
            section = fit_section(row);
            const auto* nm = section_names[section];
            draw->AddText(body, 11 * scale, ImVec2(a.x + pad + 4 * scale, y + 6 * scale), theme::muted, nm);
            const auto w = body->CalcTextSizeA(11 * scale, FLT_MAX, 0, nm).x;
            draw->AddLine(ImVec2(a.x + pad + 12 * scale + w, y + 13 * scale), ImVec2(b.x - pad, y + 13 * scale), IM_COL32(255, 255, 255, 28),
                          1.0f * scale);
            y += section_h;
        }
        const auto& r = rows[static_cast<std::size_t>(i)];
        const bool picked = i == f.row;
        if (picked) draw->AddRectFilled(ImVec2(a.x + 8 * scale, y), ImVec2(b.x - 8 * scale, y + row_h - 4 * scale), theme::blue, 4 * scale);
        const auto ink = picked ? skate_theme::black : theme::paper;
        const auto soft = picked ? IM_COL32(0, 0, 0, 150) : IM_COL32(255, 255, 255, 120);
        const float cy = y + (row_h - 4 * scale) * 0.5f;
        row_icon(draw, row, ImVec2(a.x + pad + 12 * scale, cy), 16 * scale, picked ? ink : IM_COL32(255, 255, 255, 200), r.sw, pick.quiet);
        draw->AddText(heading, 15 * scale, ImVec2(a.x + pad + 30 * scale, cy - 8.5f * scale), ink, r.label.c_str());
        float right = b.x - pad - 6 * scale;
        const auto chevrons = [&](float x0, float x1) {   // the selected row's choice, between two drawn chevrons
            const float k = 4 * scale;
            draw->AddTriangleFilled(ImVec2(x0, cy), ImVec2(x0 + k, cy - k * 1.2f), ImVec2(x0 + k, cy + k * 1.2f), ink);
            draw->AddTriangleFilled(ImVec2(x1, cy), ImVec2(x1 - k, cy - k * 1.2f), ImVec2(x1 - k, cy + k * 1.2f), ink);
        };
        switch (r.shown) {
        case Shown::text:
        case Shown::number:
        case Shown::swatch: {
            const auto ext = body->CalcTextSizeA(14 * scale, FLT_MAX, 0, r.value.c_str());
            float x1 = right - (picked && r.shown != Shown::number ? 12 * scale : 0);
            float x0 = x1 - ext.x;
            draw->AddText(body, 14 * scale, ImVec2(x0, cy - 8 * scale), ink, r.value.c_str());
            // fit35: a number's track starts left of the widest value it can show ("-10.0 cm" / "-60 deg"), not of
            // this one, so it no longer moves sideways with every step
            if (r.shown == Shown::number) x0 = x1 - body->CalcTextSizeA(14 * scale, FLT_MAX, 0, "-10.0 cm").x;
            if (r.shown == Shown::swatch) {
                x0 -= 16 * scale;
                draw->AddCircleFilled(ImVec2(x0 + 6 * scale, cy), 6.5f * scale, r.sw, 18);
                draw->AddCircle(ImVec2(x0 + 6 * scale, cy), 6.5f * scale, soft, 18, 1.2f * scale);
            }
            if (r.shown == Shown::number) {   // the range: a track, its zero, the value's knob
                const float tw = 64 * scale, tx1 = x0 - 12 * scale, tx0 = tx1 - tw;
                draw->AddLine(ImVec2(tx0, cy), ImVec2(tx1, cy), soft, 2 * scale);
                const float zero = tx0 + tw * (0 - r.lo) / (r.hi - r.lo);
                draw->AddLine(ImVec2(zero, cy - 4 * scale), ImVec2(zero, cy + 4 * scale), soft, 1.5f * scale);
                const float kx = tx0 + tw * std::clamp((r.v - r.lo) / (r.hi - r.lo), 0.0f, 1.0f);
                draw->AddLine(ImVec2(zero, cy), ImVec2(kx, cy), ink, 2 * scale);
                draw->AddCircleFilled(ImVec2(kx, cy), 4.5f * scale, ink, 14);
            } else if (picked) {
                chevrons(x0 - 12 * scale, right + 2 * scale);
            }
            break;
        }
        case Shown::toggle: {   // a switch
            const float w = 30 * scale, h = 16 * scale, x0 = right - w;
            const ImVec2 p0(x0, cy - h / 2), p1(right, cy + h / 2);
            if (r.on) draw->AddRectFilled(p0, p1, picked ? skate_theme::black : theme::blue, h / 2);
            else draw->AddRect(p0, p1, soft, h / 2, 0, 1.5f * scale);
            const float kx = r.on ? right - h / 2 : x0 + h / 2;
            draw->AddCircleFilled(ImVec2(kx, cy), h / 2 - 3 * scale, r.on ? (picked ? theme::blue : theme::paper) : soft, 14);
            const char* word = r.on ? "On" : "Off";
            const auto ext = body->CalcTextSizeA(13 * scale, FLT_MAX, 0, word);
            draw->AddText(body, 13 * scale, ImVec2(x0 - 8 * scale - ext.x, cy - 7.5f * scale), soft, word);
            break;
        }
        case Shown::action:
            if (picked) theme::keycap(draw, body, scale, ImVec2(right - 20 * scale, cy - 10 * scale), p.pad_prompts.load() ? "A" : "Enter", "");
            break;
        }
        y += row_h;
    }
    // the footer: the two keys that matter (the D-pad is shown by the chevrons and the tracks)
    float x = a.x + pad;
    const bool pad_keys = p.pad_prompts.load();
    const auto prompt = [&](const char* key, const char* label) {
        x += theme::keycap(draw, body, scale, ImVec2(x, y + 8 * scale), key, label) + 18 * scale;
    };
    prompt(pad_keys ? "A" : "Enter", "Pick");
    prompt(pad_keys ? "B" : "Esc", "Close");
}

bool item_browser_open() { return panel().view.open; }

void item_browser_key(ItemBrowserKey key) {
    const auto* h = host();
    if (!h) return;
    if (key == ItemBrowserKey::favorite && h->toggle_favorite) h->toggle_favorite();
    if (key == ItemBrowserKey::filter && h->cycle_filter) h->cycle_filter();
    if (key == ItemBrowserKey::previous_tab && h->step_tab) h->step_tab(-1);
    if (key == ItemBrowserKey::next_tab && h->step_tab) h->step_tab(1);
    if (key == ItemBrowserKey::previous_tab || key == ItemBrowserKey::next_tab) panel().pad_prompts = false;
}

void item_browser_search_cleared() {
    auto& p = panel();
    p.input.fill(0);
    if (const auto* h = host(); h && h->set_search) h->set_search("");
}

bool item_browser_pending() {
    auto& s = state();
    auto& p = panel();
    const auto* h = host();
    if (!h || !h->view) return false;
    // Every frame: the card under the game's item card follows the highlight without a lag.
    try { p.view = h->view(); } catch (...) { p.view = {}; }
    if (!p.view.open) {
        if (p.fit.open || p.fit.capture) {
            dingosdk::ControllerInput pad;
            DingoSDKOverlayReadControllerInput(&pad);
            fit_close(p.fit);
            const bool any = (pad.available && pad.buttons) ||
                             std::any_of(pad.keys.begin(), pad.keys.end(), [](std::uint64_t bits) { return bits != 0; });
            if (!any) p.fit.release = false;
            fit_capture(p.fit, p.fit.release);
        }
        s.item_search_visible.store(false);
        // A button still held when a grid takes the focus again counts once it has been let go.
        p.stick_down = p.previous_down = p.next_down = true;
        p.click_down = false;
        p.bar.shown = false;
        return false;
    }
    // Left stick click: favorite. Read here, once a presented frame, only while a grid is open.
    dingosdk::ControllerInput pad;
    DingoSDKOverlayReadControllerInput(&pad);
    update_fit(p, pad);
    if (p.fit.open || p.fit.capture) {
        p.stick_down = p.previous_down = p.next_down = true;
        return true;
    }
    const bool stick = pad.available && (pad.buttons & XINPUT_GAMEPAD_LEFT_THUMB) != 0;
    if (stick && !p.stick_down) item_browser_key(ItemBrowserKey::favorite);
    p.stick_down = stick;
    // The prompts follow the last thing used, as the game's own do.
    if (pad.available && pad.buttons) p.pad_prompts = true;
    else if (std::any_of(pad.keys.begin(), pad.keys.end(), [](std::uint64_t bits) { return bits != 0; })) p.pad_prompts = false;

    // The type tabs: LT and RT step through them (kept from the game while the tabs show), and the
    // mouse can pick one.
    // A press is told by the trigger itself, whether or not the tabs are up at that moment: a held trigger
    // never steps again on its own.
    const bool tabs = p.view.tabs.size() >= 3;
    const auto now = Clock::now();
    if (tabs) p.tabs_seen = now;
    const bool previous = pad.available && (pad.buttons & 0x10000) != 0;
    const bool next = pad.available && (pad.buttons & 0x20000) != 0;
    if (tabs || now - p.tabs_seen < std::chrono::milliseconds(500)) hold_item_browser_triggers();
    if (tabs && previous && !p.previous_down && h->step_tab) h->step_tab(-1);
    if (tabs && next && !p.next_down && h->step_tab) h->step_tab(1);
    p.previous_down = previous;
    p.next_down = next;

    p.mouse = ImVec2(-1, -1);
    bool click = false;
    if (const HWND window = s.window.load(); window && tabs && p.bar.shown) {
        POINT cursor{};
        RECT client{};
        {
            OverlayInputAccess access;
            click = (GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) & 0x8000) != 0;
            if (!GetCursorPos(&cursor)) cursor = {-1, -1};
        }
        if (ScreenToClient(window, &cursor) && GetClientRect(window, &client) && client.right > 0 && client.bottom > 0)
            p.mouse = ImVec2(static_cast<float>(cursor.x) * p.bar.display.x / static_cast<float>(client.right),
                             static_cast<float>(cursor.y) * p.bar.display.y / static_cast<float>(client.bottom));
        if (click && !p.click_down && h->pick_tab && h->step_tab) {
            const auto& bar = p.bar;
            const float x = p.mouse.x, y = p.mouse.y;
            if (y >= bar.a.y && y <= bar.b.y) {
                if (x >= bar.previous_x0 && x <= bar.previous_x1) h->step_tab(-1);
                else if (x >= bar.next_x0 && x <= bar.next_x1) h->step_tab(1);
                else if (x >= bar.clip_x0 && x <= bar.clip_x1)
                    for (std::size_t i = 0; i < bar.tabs.size(); ++i)
                        if (x >= bar.tabs[i].first && x <= bar.tabs[i].second) h->pick_tab(static_cast<unsigned>(i));
            }
        }
    }
    p.click_down = click;
    return true;
}

void draw_item_browser() {
    auto& s = state();
    auto& p = panel();
    const auto* h = host();
    if (!h || !p.view.open) return;
    const auto& view = p.view;
    auto* heading = s.menu.bold ? s.menu.bold : ImGui::GetFont();
    auto* body = s.menu.body ? s.menu.body : ImGui::GetFont();
    const auto display = ImGui::GetIO().DisplaySize;
    if (display.y > 1) p.aspect = display.x / display.y;
    // Sized like the game's own item card and prompts, which are laid out for a 2160-line screen.
    const float scale = std::clamp(display.y / 1080.0f, 0.75f, 3.0f) * 1.35f;
    const float width = 350.0f * scale, height = 42.0f * scale, margin = 0.074f * display.y;
    // At the right of the game's menu, which stops widening at 20:9.
    const auto [frame_left, frame_width] = menu_frame(display);
    const ImVec2 at(frame_left + frame_width - margin - width, margin);
    const bool typing = s.item_search_visible.load();

    // The search field: a dark tile like the game's item card, blue-edged while it takes the keys.
    auto* draw = ImGui::GetBackgroundDrawList();
    draw_tabs(draw, p, heading, body, display);
    skate_theme::rough_rect(draw, at, ImVec2(at.x + width, at.y + height), IM_COL32(18, 18, 19, 235), 71, scale);
    if (typing) draw->AddRect(ImVec2(at.x - scale, at.y - scale), ImVec2(at.x + width + scale, at.y + height + scale), theme::blue, 0, 0, 2.5f * scale);
    const ImVec2 lens(at.x + 24 * scale, at.y + height * 0.46f);
    draw->AddCircle(lens, 8 * scale, theme::paper, 0, 2.2f * scale);
    draw->AddLine(ImVec2(lens.x + 6 * scale, lens.y + 6 * scale), ImVec2(lens.x + 12 * scale, lens.y + 12 * scale), theme::paper, 2.6f * scale);
    const std::string count = view.shown == view.total ? std::to_string(view.total)
                                                        : std::to_string(view.shown) + " / " + std::to_string(view.total);
    const float count_size = 15 * scale;
    const auto count_extent = body->CalcTextSizeA(count_size, FLT_MAX, 0, count.c_str());
    draw->AddText(body, count_size, ImVec2(at.x + width - count_extent.x - 14 * scale, at.y + (height - count_extent.y) * 0.5f),
                  theme::muted, count.c_str());

    const float text_x = at.x + 46 * scale, text_width = width - 46 * scale - count_extent.x - 28 * scale;
    if (typing) {
        ImGui::SetNextWindowPos(ImVec2(text_x, at.y), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(text_width, height), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, (height - 20 * scale) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, theme::paper);
        ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, IM_COL32(1, 131, 255, 110));
        constexpr auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground;
        if (ImGui::Begin("##reskate_item_search", nullptr, flags)) {
            ImGui::PushFont(heading);
            ImGui::SetWindowFontScale(20 * scale / heading->FontSize);
            if (s.item_search_focus_requested.exchange(false)) {
                std::snprintf(p.input.data(), p.input.size(), "%s", view.search.c_str());
                const auto id = ImGui::GetID("##reskate_item_search_input");
                if (auto* active = ImGui::GetInputTextState(id); active && ImGui::GetActiveID() == id)
                    active->ReloadUserBufAndMoveToEnd();
                ImGui::SetKeyboardFocusHere();
            }
            ImGui::SetNextItemWidth(-FLT_MIN);
            const bool entered = ImGui::InputTextWithHint("##reskate_item_search_input", "Type to search", p.input.data(),
                                                          p.input.size(), ImGuiInputTextFlags_EnterReturnsTrue);
            // Every letter filters the grid at once.
            if (h->set_search && p.pushed != p.input.data()) {
                p.pushed = p.input.data();
                h->set_search(p.pushed.c_str());
            }
            if (entered) s.item_search_visible.store(false);
            ImGui::PopFont();
        }
        ImGui::End();
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar(3);
    } else {
        p.pushed = view.search;
        const float size = 20 * scale;
        const auto text = view.search.empty() ? std::string("Search") : view.search;
        draw->PushClipRect(ImVec2(text_x, at.y), ImVec2(text_x + text_width, at.y + height), true);
        draw->AddText(heading, size, ImVec2(text_x, at.y + (height - size) * 0.5f - scale),
                      view.search.empty() ? theme::muted : theme::paper, text.c_str());
        draw->PopClipRect();
    }

    // Key prompts, like the game's "Esc Back".
    float x = at.x;
    const float y = at.y + height + 12 * scale;
    const auto prompt = [&](const char* key, const std::string& label) {
        x += theme::keycap(draw, body, scale, ImVec2(x, y), key, label.c_str()) + 22 * scale;
    };
    if (typing) {
        prompt("Enter", "Done");
        prompt("Esc", "Clear");
    } else {
        prompt("F", view.focused_favorite ? "Unfavorite" : "Favorite");
        prompt("-", "Search");
        prompt("X", view.filter);
        if (!acc::model_pick(view.focused_key).model.empty()) prompt(p.pad_prompts.load() ? "R3" : "V", "Customize");
    }
    if (view.focused_favorite && !typing)
        star(draw, ImVec2(at.x - 22 * scale, at.y + height * 0.5f), 13 * scale, skate_theme::bar);
    draw_mod_card(draw, view, heading, body);
    if (p.fit.open) draw_fit(draw, p, heading, body, scale, ImVec2(at.x, y + 36 * scale), width);
}
} // namespace dingosdk::overlay::detail
