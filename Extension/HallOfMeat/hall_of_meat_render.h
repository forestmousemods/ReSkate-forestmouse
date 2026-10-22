#pragma once
#include "Engine/Game/Abi/linear_transform.h"
#include <array>
#include <cstdint>
#include <vector>

// The local skater as the renderer draws it (Engine/Game/Build/20260929/skater_render.h): the
// skinning matrices its mesh was drawn with and the camera of the same picture, so Hall of Meat's
// skeleton lies exactly where the game draws the skater. Two hooks that only observe: the skinned
// draw packets, and the main view's camera.
namespace dingosdk::hall_of_meat {
// Requires the validated build: verifies the layout and prepares both hooks, which patches nothing.
bool start_render(std::uintptr_t image_base) noexcept;
// Client tick: both hooks on while Hall of Meat is, off otherwise, so a renderer it is switched off
// for never calls it. False when they could not be changed (logged; both stay as they were).
bool hook_render(bool on) noexcept;
// Client tick: while `wanted` (a bail shows), which render objects are the local skater's
// (no_bail.h no_bail_skater), riding and walking; otherwise both hooks pass every call straight on.
void follow_render(bool wanted) noexcept;

struct Picture {
    std::vector<game::LinearTransform> skin; // each render bone's skinning matrix: from model space into the world
    std::array<float, 16> camera{};          // the camera's world matrix: right, up, back, position rows
    float vertical_fov{};                    // degrees
};
// Render thread: the latest picture's, while the renderer draws the local skater.
bool latest_picture(Picture& picture) noexcept;

// The accessory customizer (Extension/Customization/accessory_probe.h) shares the draw packet hook:
// it may hand the renderer its own copy of a packet's bones. Its hooks are on while either wants
// them (hook_render is Hall of Meat's own switch). Client tick.
bool hook_render_for_accessories(bool on) noexcept;
// Render threads, after each skinned draw packet the hooks see (nullptr: none).
using PacketEditor = void (*)(std::uintptr_t object, std::uintptr_t packet) noexcept;
void set_packet_editor(PacketEditor editor) noexcept;
// Render threads, before each rendered view is copied for the shaders (nullptr: none): may change its camera, and the
// previous view's (what motion blur and temporal filters compare with). The camera rows (+0x40) and field of view
// (+0x104) it changes are put back once the view is drawn, so the game never reads the edit back.
using ViewEditor = void (*)(std::uintptr_t view, std::uintptr_t previous) noexcept;
void set_view_editor(ViewEditor editor) noexcept;
// The render index of the local skater's render object for a mode (0 riding, 1 walking), or UINT32_MAX.
std::uint32_t skater_render_index(std::uintptr_t entity, std::size_t mode) noexcept;
}
