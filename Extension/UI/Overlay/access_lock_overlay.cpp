#include "overlay_internal.h"
#include "Extension/Customization/accessory_probe.h"
#include <algorithm>
#include <array>

// Physical Lanyards' lock notice (forestmouse fit60): while the mod is applied on a PC whose Discord account is on its
// lock list (accessory_probe access_locked), the whole screen is covered with a notice saying so and how to turn the
// mod off. Drawn over everything, every presented frame; it takes no input, so the game's own menus still work.
namespace dingosdk::overlay::detail {

bool access_lock_pending() { return dingosdk::accessory_probe::access_locked(); }

void draw_access_lock() {
    if (!access_lock_pending()) return;
    const auto display = ImGui::GetIO().DisplaySize;
    if (display.x < 2 || display.y < 2) return;
    auto& s = state();
    auto* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(ImVec2(0, 0), display, IM_COL32(10, 10, 12, 255));
    auto* heading = s.menu.heading ? s.menu.heading : ImGui::GetFont();
    auto* body = s.menu.body ? s.menu.body : ImGui::GetFont();
    const float scale = std::clamp(display.y / 1080.0f, 0.6f, 3.0f);
    struct Line { ImFont* font; float size; ImU32 colour; const char* text; };
    const std::array<Line, 5> lines{{
        {heading, 46.0f * scale, IM_COL32(255, 255, 255, 255), dingosdk::accessory_probe::access_lock_title()},
        {body, 26.0f * scale, IM_COL32(235, 235, 235, 255), dingosdk::accessory_probe::access_lock_line()},
        {body, 14.0f * scale, 0, ""},
        {body, 21.0f * scale, IM_COL32(170, 170, 175, 255), "Turn Physical_Lanyards off in the ReSkate launcher (Mods) and start the game again."},
        {body, 14.0f * scale, 0, ""},
    }};
    float total = 0.0f;
    for (const auto& l : lines) total += l.size * 1.35f;
    float y = (display.y - total) * 0.5f;
    for (const auto& l : lines) {
        if (*l.text) {
            const ImVec2 extent = l.font->CalcTextSizeA(l.size, FLT_MAX, 0.0f, l.text);
            draw->AddText(l.font, l.size, ImVec2((display.x - extent.x) * 0.5f, y), l.colour, l.text);
        }
        y += l.size * 1.35f;
    }
}
}  // namespace dingosdk::overlay::detail
