#include "accessory_probe.h"
#include "item_browser_order.h"
#include "local_customization_runtime.h"
#include "developer_hoodie_material.h"
#include "developer_board_material.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/skater_render.h"
#include "Engine/Game/Rendering/draw_packet.h"
#include "Extension/HallOfMeat/hall_of_meat_render.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include "Engine/Game/UI/game_view.h"
#include "Extension/UI/Overlay/overlay.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/Boot/discord_presence.h"
#include "Extension/Assets/live_mods.h"
#include <Windows.h>
#include <bcrypt.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstdlib>
#include <optional>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dingosdk::accessory_probe {
namespace {
namespace render = addr::skater_render;
using Packed = draw_packet::PackedBone;
using developer_hoodie_detail::Color;
constexpr std::uint32_t no_index = UINT32_MAX;
constexpr std::uint32_t most_bones = 1024;
constexpr std::size_t ring_size = 64;      // copies the renderer may still read while new frames are packed
constexpr std::size_t snapshot_slots = 64; // render objects kept, each sampled on its own
constexpr std::uint64_t sample_ms = 250;   // how often one object's palette is copied
constexpr std::uint64_t lease_ms = 500;    // the skater's render indices, refreshed by the client tick
constexpr std::size_t raw_packet = 0x100, raw_object = 0x200, raw_target = 0x600;

// Our renderbones asset (gen_overalls_overallsboilersuit_renderbones as Physical_Lanyards 1.3.0 extends it):
// 296 bones, mesh blend index 0x8000 + i = bone i. The braided keychains' top ring KD at the back, front and
// side of each thigh is static (P_STATIC on the native graph) on RM_Left/RightUpLeg_03, so the three of a side
// keep their distances: that finds the asset's bones in a palette. Bind positions in model space (build_combo
// report bones_xyz).
constexpr std::size_t asset_bones = 0x128;
struct Anchor { std::uint16_t index; std::array<float, 3> bind; };
constexpr std::array<Anchor, 3> left_anchors{{{0x2c, {0.1797f, 0.8553f, -0.0654f}},
                                              {0x36, {0.1002f, 0.8456f, 0.1538f}},
                                              {0x40, {0.2094f, 0.8545f, -0.0002f}}}};
constexpr std::array<Anchor, 3> right_anchors{{{0x4a, {-0.1797f, 0.8553f, -0.0654f}},
                                               {0x54, {-0.1002f, 0.8456f, 0.1538f}},
                                               {0x5e, {-0.2094f, 0.8545f, -0.0002f}}}};
constexpr std::size_t anchors_need = 0x5f;
constexpr float tolerance = 0.004f; // metres

struct Snapshot {
    std::uintptr_t object{}, vtable{};
    std::uint32_t index{}, count{};
    std::uint8_t relative{}, visible{};
    std::array<float, 4> root{};
    std::array<float, 16> actor{};
    std::uint64_t at{};
    std::vector<Packed> bones; // most_bones reserved
};
struct Raw {
    std::uintptr_t object{}, packet{};
    std::array<std::uint8_t, raw_packet> packet_bytes{};
    std::array<std::uint8_t, raw_object> object_bytes{};
    bool filled{};
};
struct SavedMode { std::uintptr_t material{}; std::array<std::uint8_t, 16> original{}; };
struct TestSaved { std::uintptr_t material{}; std::uint64_t key{}; developer_hoodie_detail::Color original{}; };
struct Saved {
    std::uintptr_t item{}, material{}, node{};
    std::uint64_t key{};
    Color original{};
};

struct State {
    std::atomic<std::uintptr_t> base{};
    std::atomic<int> color{}, region{-1}, mode{1}, part{6};
    std::atomic<float> lift{};
    std::atomic<bool> params{}, scan_wanted{}, test_wanted{}, finish_unsaved{};
    std::atomic<std::uint64_t> prepare_until{}; // a keychain of ours is picked in a grid: have its copies found
    // The keychain panel's camera: looks at the keychain at this place while focus_until is ahead (focus_blend 0..1).
    std::atomic<int> focus_place{-1};
    std::atomic<float> turn_error{};                 // fit52: the panel's turn to face the keychain (radians)
    std::atomic<std::uint64_t> turn_error_at{};
    std::atomic<std::uint64_t> focus_until{};
    float focus_blend{};
    std::uint64_t focus_at{};
    std::atomic<float> focus_aspect{16.0f / 9.0f};
    std::array<float, 6> fov_seen{};   // the first frames' field of view, for the log
    int fov_seen_count{};
    bool fov_logged{};
    bool test_running{};
    std::uint64_t test_began{};
    int test_shown{-1};
    std::uint8_t test_settle{};
    std::vector<TestSaved> test_saved;
    std::vector<SavedMode> test_modes;
    std::atomic<int> finish{}, finish_mode{-1};
    std::atomic<float> brightness{1.0f};
    SRWLOCK hips_lock = SRWLOCK_INIT;
    // The hips of every skinned skeleton drawn lately (386 bones), for the panel's camera: the wardrobe's skater is a
    // preview character of its own, the one nearest the camera.
    struct Body { std::uintptr_t object{}; Packed hips{}; std::uint64_t at{}; };
    std::array<Body, 8> bodies{};
    std::size_t next_body{};
    Packed hips_bone{};
    std::uint64_t hips_at{};
    std::vector<Saved> finish_colors;
    std::vector<SavedMode> finish_modes;
    bool finish_active{};
    int finish_logged{-1};
    std::uint8_t finish_settle{};
    // fit34: the strap's colour (the keychain's second material, Outfit_Bottom_mat), kept per keychain like the finish
    std::atomic<int> strap{};
    std::atomic<bool> strap_unsaved{}, has_strap{};
    std::atomic<std::uint32_t> charms{};   // fit36: the charm slots' picks (charm_pack; 0 = the defaults)
    std::atomic<bool> charm_unsaved{};
    std::vector<Saved> strap_colors;
    bool strap_active{};
    int strap_logged{-1};
    std::uint64_t strap_looked{};
    std::atomic<int> colormode{INT_MIN};
    std::array<std::uint8_t, 16> colormode_original{};
    bool colormode_saved{};
    std::atomic<bool> dump{}, status{}, armed{}, want_raw{};
    std::array<std::atomic<std::uint32_t>, render::render_modes> indices{no_index, no_index};
    std::atomic<std::uint64_t> indices_until{};
    // render threads -> client thread
    SRWLOCK lock = SRWLOCK_INIT;
    std::array<Snapshot, snapshot_slots> snapshots;
    std::size_t next_snapshot{};
    Raw raw;
    // client thread -> render threads: where our asset's bones are
    std::array<std::atomic<std::uintptr_t>, snapshot_slots> found_object{};
    std::array<std::atomic<std::uint32_t>, snapshot_slots> found_count{};
    std::array<std::atomic<int>, snapshot_slots> found_block{};
    std::array<std::vector<Packed>, ring_size> ring;
    std::atomic<unsigned> next_ring{};
    std::atomic<std::uint64_t> packets{}, ours{}, skater_packets{}, redirected{}, in_place{}, skipped{};
    // client thread
    bool hooked{}, prepared{};
    std::vector<Saved> saved;          // the outfit item's own colours, while we colour it
    int applied_color{}, applied_region{-1};
    std::size_t applied_part{6};
    std::uint8_t settle{};
    std::uintptr_t colored_item{};
    std::string last_bindings, last_found;
};
State& state() { static auto* value = new State; return *value; }

std::filesystem::path folder() {
    static const std::filesystem::path path = [] {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        return std::filesystem::path(exe).parent_path() / L"logs" / L"accessory_probe";
    }();
    return path;
}
// ReSkate.log is overwritten when the launcher starts again: every line also goes to our own file.
void note(const std::string& line) noexcept {
    try {
        logging::log(logging::Level::info, logging::Channel::customization, "Accessories: {}", line);
        std::filesystem::create_directories(folder());
        SYSTEMTIME t{};
        GetLocalTime(&t);
        std::ofstream(folder() / std::format(L"acc_{:04}{:02}{:02}.log", t.wYear, t.wMonth, t.wDay), std::ios::app)
            << std::format("{:02}:{:02}:{:02}.{:03} ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds) << line << '\n';
    } catch (...) {}
}

// The packet-frame position of a bind position through a packed skinning matrix (3 rows of 4).
std::array<float, 3> through(const Packed& p, const std::array<float, 3>& b) noexcept {
    return {p[0] * b[0] + p[1] * b[1] + p[2] * b[2] + p[3], p[4] * b[0] + p[5] * b[1] + p[6] * b[2] + p[7],
            p[8] * b[0] + p[9] * b[1] + p[10] * b[2] + p[11]};
}
float distance(const std::array<float, 3>& a, const std::array<float, 3>& b) noexcept {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}
bool rigid(const Packed& p) noexcept {
    for (std::size_t column = 0; column < 3; ++column) {
        const float length = std::sqrt(p[column] * p[column] + p[4 + column] * p[4 + column] + p[8 + column] * p[8 + column]);
        if (!std::isfinite(length) || std::abs(length - 1.0f) > 0.05f) return false;
    }
    return true;
}
template<std::size_t N> bool keeps_shape(const std::vector<Packed>& bones, std::size_t block, const std::array<Anchor, N>& anchors) noexcept {
    std::array<std::array<float, 3>, N> at{};
    for (std::size_t i = 0; i < N; ++i) {
        const auto& p = bones[block + anchors[i].index];
        if (!rigid(p)) return false;
        at[i] = through(p, anchors[i].bind);
    }
    for (std::size_t i = 0; i < N; ++i)
        for (std::size_t j = i + 1; j < N; ++j)
            if (std::abs(distance(at[i], at[j]) - distance(anchors[i].bind, anchors[j].bind)) > tolerance) return false;
    return true;
}
// Where our asset's bones begin in a palette, or -1; several fits are reported as -2.
int find_block(const std::vector<Packed>& bones, std::uint32_t count) noexcept {
    int found = -1;
    for (std::size_t block = 0; block + anchors_need <= count; ++block)
        if (keeps_shape(bones, block, left_anchors) && keeps_shape(bones, block, right_anchors)) {
            if (found >= 0) return -2;
            found = static_cast<int>(block);
        }
    return found;
}

// Render threads, after each skinned draw packet.
void edit_packet(std::uintptr_t object, std::uintptr_t packet) noexcept {
    auto& s = state();
    const auto base = s.base.load(std::memory_order_acquire);
    if (!base) return;
    const bool armed = s.armed.load(std::memory_order_acquire);
    if (!armed && GetTickCount64() > s.focus_until.load(std::memory_order_acquire) + 2000) return;
    __try {
        s.packets.fetch_add(1, std::memory_order_relaxed);
        const auto vtable = *reinterpret_cast<std::uintptr_t*>(object);
        const bool ours = vtable == base + render::render_object_vtable;
        if (ours) s.ours.fetch_add(1, std::memory_order_relaxed);
        const auto count = *reinterpret_cast<std::uint32_t*>(packet + render::packet_bone_count_offset);
        const auto visible = *reinterpret_cast<std::uint8_t*>(packet + render::packet_visible_offset);
        const auto bones = *reinterpret_cast<std::uintptr_t*>(packet + render::packet_bones_offset);
        const auto index = ours ? *reinterpret_cast<std::uint32_t*>(object + render::render_index_offset) : no_index;
        const auto locked = ours ? *reinterpret_cast<std::uint32_t*>(object + render::render_object_lock_offset) : 0u;
        if (locked || !bones || !count || count > most_bones) return;
        const auto now = GetTickCount64();
        const bool skater = ours && now <= s.indices_until.load(std::memory_order_acquire) &&
                            (index == s.indices[0].load(std::memory_order_acquire) || index == s.indices[1].load(std::memory_order_acquire));
        if (skater) s.skater_packets.fetch_add(1, std::memory_order_relaxed);
        // The skater's hips as drawn, for the chameleon finishes' viewing angle; every skeleton's for the panel's camera.
        if (ours && count == 386 && TryAcquireSRWLockExclusive(&s.hips_lock)) {
            if (skater) {
                std::memcpy(s.hips_bone.data(), reinterpret_cast<void*>(bones + 7 * sizeof(Packed)), sizeof(Packed));
                s.hips_at = now;
            }
            std::size_t slot = s.bodies.size();
            for (std::size_t i = 0; i < s.bodies.size(); ++i)
                if (s.bodies[i].object == object) slot = i;
            if (slot == s.bodies.size()) slot = s.next_body, s.next_body = (s.next_body + 1) % s.bodies.size();
            s.bodies[slot].object = object;
            std::memcpy(s.bodies[slot].hips.data(), reinterpret_cast<void*>(bones + 7 * sizeof(Packed)), sizeof(Packed));
            s.bodies[slot].at = now;
            ReleaseSRWLockExclusive(&s.hips_lock);
        }
        if (!armed) return;
        // Every object's palette now and then, for the client thread's search (any vtable: the draw packet
        // contract is the same for every skinned render object that hands one out).
        if (TryAcquireSRWLockExclusive(&s.lock)) {
            std::size_t slot = snapshot_slots;
            for (std::size_t i = 0; i < snapshot_slots; ++i)
                if (s.snapshots[i].object == object) { slot = i; break; }
            if (slot == snapshot_slots) {
                slot = s.next_snapshot;
                s.next_snapshot = (s.next_snapshot + 1) % snapshot_slots;
                s.snapshots[slot].at = 0;
            }
            auto& shot = s.snapshots[slot];
            if (now - shot.at >= sample_ms) {
                shot.object = object, shot.vtable = vtable - base, shot.index = index, shot.count = count, shot.at = now;
                shot.relative = *reinterpret_cast<std::uint8_t*>(packet + render::packet_relative_offset);
                shot.visible = visible;
                const auto root = *reinterpret_cast<float**>(packet + render::packet_root_offset);
                shot.root = {};
                if (root) std::memcpy(shot.root.data(), root, sizeof(shot.root));
                std::memcpy(shot.actor.data(), reinterpret_cast<void*>(packet + render::packet_actor_offset), sizeof(shot.actor));
                shot.bones.resize(count);
                std::memcpy(shot.bones.data(), reinterpret_cast<void*>(bones), count * sizeof(Packed));
            }
            if (skater && s.want_raw.load(std::memory_order_acquire) && !s.raw.filled) {
                s.raw.object = object, s.raw.packet = packet;
                std::memcpy(s.raw.packet_bytes.data(), reinterpret_cast<void*>(packet), raw_packet);
                std::memcpy(s.raw.object_bytes.data(), reinterpret_cast<void*>(object), raw_object);
                s.raw.filled = true;
            }
            ReleaseSRWLockExclusive(&s.lock);
        }
        // The lift: our asset's bones moved along the left top ring's up, in our copy or in place.
        const float lift = s.lift.load(std::memory_order_acquire);
        if (lift == 0.0f) return;
        int block = -1;
        for (std::size_t i = 0; i < snapshot_slots; ++i)
            if (s.found_object[i].load(std::memory_order_acquire) == object && s.found_count[i].load(std::memory_order_acquire) == count) {
                block = s.found_block[i].load(std::memory_order_acquire);
                break;
            }
        if (block < 0) return;
        const std::size_t end = std::min<std::size_t>(block + asset_bones, count);
        auto apply = [&](Packed* p) {
            const auto& a = p[block + left_anchors[0].index];
            const float add[3]{a[1] * lift, a[5] * lift, a[9] * lift}; // the anchor's up (its y column) times the lift
            for (std::size_t bone = block; bone < end; ++bone) {
                p[bone][3] += add[0];
                p[bone][7] += add[1];
                p[bone][11] += add[2];
            }
        };
        if (s.mode.load(std::memory_order_acquire) == 2) {
            // In place: the object packs its bones only when they change, so a palette lifted already
            // (its top ring where we left it) is not lifted again.
            auto* p = reinterpret_cast<Packed*>(bones);
            static thread_local std::array<float, 3> written{};
            static thread_local std::uintptr_t written_object{};
            const auto& a = p[block + left_anchors[0].index];
            if (written_object == object && a[3] == written[0] && a[7] == written[1] && a[11] == written[2]) {
                s.skipped.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            apply(p);
            written = {a[3], a[7], a[11]};
            written_object = object;
            s.in_place.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        auto& copy = s.ring[s.next_ring.fetch_add(1, std::memory_order_relaxed) % ring_size];
        if (copy.size() < count) return; // reserved on the client thread before arming
        std::memcpy(copy.data(), reinterpret_cast<void*>(bones), count * sizeof(Packed));
        apply(copy.data());
        *reinterpret_cast<std::uintptr_t*>(packet + render::packet_bones_offset) = reinterpret_cast<std::uintptr_t>(copy.data());
        s.redirected.fetch_add(1, std::memory_order_relaxed);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// The colours: on the skater's own material instances of the Outfits slot, every colour parameter its
// material has (or one of them: `region`), whatever the item's preset left neutral.
constexpr std::array<std::array<std::uint8_t, 3>, 7> picks{{
    {0, 0, 0}, {0x8a, 0x2b, 0xe2}, {0x7f, 0xff, 0x00}, {0xff, 0xc0, 0x20}, {0xff, 0x2a, 0x8a}, {0x30, 0xa0, 0xff}, {0x14, 0x14, 0x14}}};
constexpr std::array<const char*, 7> pick_names{"own", "purple", "acid green", "gold", "pink", "ice blue", "black"};
constexpr std::size_t outfit_part = 6; // CAS_Outfit_Slot in developer_hoodie_detail::slots
constexpr std::uint8_t settle_publishes = 3;
// A colour parameter of a material instance: written in place, or, when the material has none with this key
// (our items' preset lacks the clothing base colour 0x8c46cd354b18a9e2 that a T-shirt has), added by the engine's own
// setter the way the developer board adds its base colour (developer_board.cpp).
bool write_color(std::uintptr_t base, const developer_hoodie_detail::Binding& binding, const Color& color) noexcept {
    __try {
        if (!binding.node) {
            const developer_board_detail::NativeColor value{color, 0, base + addr::native_cosmetics::color_rgb_type};
            return reinterpret_cast<std::uint64_t (*)(std::uintptr_t, const developer_hoodie_detail::ParameterKey*,
                                                       const developer_board_detail::NativeColor*, std::uint8_t)>(
                base + addr::native_cosmetics::set_shader_parameter)(binding.material, &binding.key, &value, 0) != 0;
        }
        std::memcpy(reinterpret_cast<void*>(binding.node + 0x10), color.data(), sizeof(color));
        *reinterpret_cast<std::uint8_t*>(binding.material + 0x2f6) = 1;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool write_color(std::uintptr_t material, std::uintptr_t node, const Color& color) noexcept {
    __try {
        std::memcpy(reinterpret_cast<void*>(node + 0x10), color.data(), sizeof(color));
        *reinterpret_cast<std::uint8_t*>(material + 0x2f6) = 1;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool publish_materials(std::uintptr_t base, std::uintptr_t item) noexcept {
    __try {
        reinterpret_cast<void (*)(std::uintptr_t, int)>(base + addr::native_cosmetics::publish_materials)(item, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void tick_colors(std::uintptr_t base, std::uintptr_t client) {
    auto& s = state();
    const int choice = s.color.load(std::memory_order_acquire), region = s.region.load(std::memory_order_acquire);
    const bool change = choice != s.applied_color || region != s.applied_region;
    if (!choice && s.saved.empty()) return;
    static std::uint64_t walked{};
    if (!change && !s.settle && GetTickCount64() - walked < 250) return;
    walked = GetTickCount64();
    std::array<unsigned char, addr::native_cosmetics::publish_materials_prefix.size()> prefix{};
    std::array<unsigned char, addr::native_cosmetics::set_shader_parameter_prefix.size()> setter{};
    if (!memory::peek_bytes(base + addr::native_cosmetics::publish_materials, prefix.data(), prefix.size()) ||
        prefix != addr::native_cosmetics::publish_materials_prefix ||
        !memory::peek_bytes(base + addr::native_cosmetics::set_shader_parameter, setter.data(), setter.size()) ||
        setter != addr::native_cosmetics::set_shader_parameter_prefix)
        return;
    const auto local = multiplayer::capture_local(base, client, false);
    if (!local.ready) return;
    auto read = [](std::uintptr_t address, void* out, std::size_t size) { return memory::peek_bytes(address, out, size); };
    const auto live = developer_hoodie_detail::materials(read, base, local.entity);
    if (live.pending) return;
    // Another slot to test on: what this one was given goes back first.
    const auto part = static_cast<std::size_t>(s.part.load(std::memory_order_acquire));
    if (part != s.applied_part) {
        std::vector<std::uintptr_t> items;
        for (const auto& b : live.bindings)
            for (const auto& saved : s.saved)
                if (saved.material == b.material && saved.key == b.key.hash && write_color(b.material, b.node, saved.original) &&
                    std::find(items.begin(), items.end(), b.item) == items.end())
                    items.push_back(b.item);
        for (const auto item : items) (void)publish_materials(base, item);
        s.saved.clear(), s.colored_item = 0, s.applied_part = part, s.last_bindings.clear(), s.applied_color = -1;
    }
    std::vector<developer_hoodie_detail::Binding> list;
    for (const auto& b : live.bindings)
        if (b.part == part) list.push_back(b);
    // The colours a T-shirt has and this item lacks: the base colour (the board's identity), and every other
    // key the worn top has (their identities read from it).
    std::uint16_t type_index{};
    (void)memory::peek(base + addr::native_cosmetics::color_rgb_type + 8, type_index);
    std::vector<developer_hoodie_detail::ParameterKey> wanted{developer_board_detail::parameter_key(developer_board_detail::base_color, type_index)};
    for (const auto& b : live.bindings)
        if (b.part == 0 && std::none_of(wanted.begin(), wanted.end(), [&](const auto& k) { return k.hash == b.key.hash; })) wanted.push_back(b.key);
    std::vector<std::uintptr_t> materials_seen;
    for (const auto& b : list)
        if (std::find(materials_seen.begin(), materials_seen.end(), b.material) == materials_seen.end()) materials_seen.push_back(b.material);
    const std::size_t own_count = list.size();
    if (part == outfit_part && !list.empty())
        for (const auto material : materials_seen)
            for (const auto& key : wanted)
                if (std::none_of(list.begin(), list.begin() + own_count, [&](const auto& b) { return b.material == material && b.key.hash == key.hash; }))
                    list.push_back({list.front().item, material, 0, key, developer_hoodie_detail::neutral, static_cast<std::uint8_t>(part)});
    std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.key.hash < b.key.hash; });
    std::string bindings;
    for (std::size_t i = 0; i < list.size(); ++i)
        bindings += std::format(" r{}:{:016x}/{:04x}@{:x}{}", i + 1, list[i].key.hash, list[i].key.identity, list[i].material & 0xfffff,
                                list[i].node ? "" : " (added)");
    if (bindings != s.last_bindings) {
        s.last_bindings = bindings;
        note(std::format("slot {} ({}) colour parameters: {} own + {} to add{}", part, developer_hoodie_detail::slots[part], own_count,
                         list.size() - own_count, bindings));
    }
    // Another item (or the same one streamed again): what was saved belongs to nothing on screen.
    const auto item = list.empty() ? 0 : list.front().item;
    if (item != s.colored_item) s.saved.clear(), s.colored_item = item;
    const auto target = choice > 0 ? developer_hoodie_detail::material_color(picks[choice]) : Color{};
    bool wrote{};
    std::string written;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const auto& b = list[i];
        auto saved = std::find_if(s.saved.begin(), s.saved.end(), [&](const Saved& x) { return x.material == b.material && x.key == b.key.hash; });
        const bool selected = choice > 0 && (region < 0 || region == static_cast<int>(i));
        if (selected && saved == s.saved.end()) {
            s.saved.push_back({b.item, b.material, b.node, b.key.hash, b.node ? b.color : developer_hoodie_detail::neutral});
            saved = s.saved.end() - 1;
        }
        if (saved == s.saved.end()) continue;
        if (b.node) saved->node = b.node;
        const auto want = selected ? target : saved->original;
        if ((!b.node || b.color != want) && write_color(base, b, want)) {
            wrote = true;
            written += std::format(" r{}{}", i + 1, b.node ? "" : "+");
        }
        if (!selected && b.node && b.color == saved->original) s.saved.erase(saved);
    }
    if (change) {
        note(std::format("colour {} ({}) on {}:{}", choice, pick_names[choice], region < 0 ? std::string("every region") :
                         std::format("region {}", region + 1), written.empty() ? std::string(" nothing written") : written));
        s.applied_color = choice, s.applied_region = region;
        s.settle = settle_publishes;
    }
    if (item && (wrote || s.settle)) {
        (void)publish_materials(base, item);
        if (!wrote && s.settle) --s.settle;
        else if (wrote) s.settle = settle_publishes;
    }
}

// acc5: every parameter of the top's and our item's material instances (the colour mode decides whether the region
// colours tint: our donor's preset says 47 = full colour, the texture as it is), and the colour mode written on ours.
constexpr std::uint64_t colormode_key = 0xc4de8a24b099a97eULL; // param_pattern_colormode (donor AP ParamInfo[12])
struct NodeCopy { std::uintptr_t node{}; std::array<std::uint8_t, 0x48> bytes{}; };
std::vector<NodeCopy> parameter_nodes(std::uintptr_t material) {
    std::vector<NodeCopy> out;
    std::uintptr_t buckets{};
    std::uint32_t size{}, total{};
    if (!memory::peek(material + 0x268, buckets) || !memory::peek(material + 0x270, size) || !memory::peek(material + 0x274, total) ||
        !buckets || !size || size > 1024 || !total || total > 512)
        return out;
    std::uintptr_t stop{};
    (void)memory::peek(buckets + size * 8ULL, stop);
    for (std::uint32_t bucket = 0; bucket < size && out.size() < total; ++bucket) {
        std::uintptr_t node{};
        (void)memory::peek(buckets + bucket * 8ULL, node);
        for (std::uint32_t visited = 0; node && node != stop && visited < total; ++visited) {
            if (std::none_of(out.begin(), out.end(), [&](const NodeCopy& c) { return c.node == node; })) {
                NodeCopy copy{node};
                if (!memory::peek_bytes(node, copy.bytes.data(), copy.bytes.size())) break;
                out.push_back(copy);
            }
            std::uintptr_t next{};
            if (!memory::peek(node + 0x40, next)) break;
            node = next;
        }
    }
    return out;
}
std::string describe(const NodeCopy& c) {
    developer_hoodie_detail::ParameterKey key{};
    std::memcpy(&key, c.bytes.data(), sizeof(key));
    std::string value;
    for (std::size_t i = 0x10; i < 0x20; ++i) value += std::format("{:02x}", c.bytes[i]);
    std::uint32_t flags{};
    std::memcpy(&flags, c.bytes.data() + 0x28, 4);
    return std::format(" [{:016x}/{:04x} size {} type {} n {} = {} fl {:x}]", key.hash, key.identity, key.size, key.type, key.count, value, flags);
}
bool write_bytes_at(std::uintptr_t address, const void* data, std::size_t size, std::uintptr_t material) noexcept {
    __try {
        std::memcpy(reinterpret_cast<void*>(address), data, size);
        *reinterpret_cast<std::uint8_t*>(material + 0x2f6) = 1;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void tick_params(std::uintptr_t base, std::uintptr_t client) {
    auto& s = state();
    const bool dump_params = s.params.exchange(false);
    const int mode = s.colormode.exchange(INT_MIN);
    if (!dump_params && mode == INT_MIN) return;
    const auto local = multiplayer::capture_local(base, client, false);
    if (!local.ready) { note("params: no local skater"); return; }
    auto read = [](std::uintptr_t address, void* out, std::size_t size) { return memory::peek_bytes(address, out, size); };
    const auto live = developer_hoodie_detail::materials(read, base, local.entity);
    std::vector<std::pair<std::size_t, std::uintptr_t>> materials;
    for (const auto& b : live.bindings)
        if ((b.part == 0 || b.part == outfit_part) &&
            std::none_of(materials.begin(), materials.end(), [&](const auto& m) { return m.second == b.material; }))
            materials.push_back({b.part, b.material});
    if (dump_params)
        for (const auto& [part, material] : materials) {
            const auto nodes = parameter_nodes(material);
            std::string line = std::format("params of slot {} material {:x}: {} nodes", part, material & 0xfffff, nodes.size());
            for (const auto& n : nodes) line += describe(n);
            note(line);
        }
    if (mode == INT_MIN) return;
    // The top's colour mode node, the value to copy.
    std::array<std::uint8_t, 16> top_value{};
    bool have_top{};
    for (const auto& [part, material] : materials)
        if (part == 0)
            for (const auto& n : parameter_nodes(material)) {
                std::uint64_t hash{};
                std::memcpy(&hash, n.bytes.data() + 8, 8);
                if (hash == colormode_key) std::memcpy(top_value.data(), n.bytes.data() + 0x10, 16), have_top = true;
            }
    std::string done;
    std::vector<std::uintptr_t> items;
    for (const auto& b : live.bindings)
        if (b.part == outfit_part && std::find(items.begin(), items.end(), b.item) == items.end()) items.push_back(b.item);
    for (const auto& [part, material] : materials) {
        if (part != outfit_part) continue;
        for (const auto& n : parameter_nodes(material)) {
            developer_hoodie_detail::ParameterKey key{};
            std::memcpy(&key, n.bytes.data(), sizeof(key));
            if (key.hash != colormode_key) continue;
            if (!s.colormode_saved) std::memcpy(s.colormode_original.data(), n.bytes.data() + 0x10, 16), s.colormode_saved = true;
            std::array<std::uint8_t, 16> value{};
            std::memcpy(value.data(), n.bytes.data() + 0x10, 16);
            if (mode == -1 && have_top) value = top_value;
            else if (mode == -2) value = s.colormode_original;
            else if (mode >= 0) { const std::uint32_t v = static_cast<std::uint32_t>(mode); std::memcpy(value.data(), &v, 4); }
            const std::size_t size = std::min<std::size_t>(key.size ? key.size : 4, 16);
            const bool ok = write_bytes_at(n.node + 0x10, value.data(), size, material);
            done += std::format(" material {:x} node {:x} size {} -> {}", material & 0xfffff, n.node & 0xfffff, size, ok ? "written" : "failed");
        }
    }
    for (const auto item : items) (void)publish_materials(base, item);
    note(std::format("colour mode {}{}:{}", mode == -1 ? std::string("like the top") : mode == -2 ? std::string("original") : std::to_string(mode),
                     mode == -1 && !have_top ? " (the top has none!)" : "", done.empty() ? std::string(" our item has no colour mode node") : done));
}

// acc6: finishes on the metal parts only. Our textures put the metal (carabiner, rings, keys, chain) in region "a"
// (msk.B 0) whose colour is param_region_color_1a; the braid is the base region, plastic charms "b". The preset's
// colour mode (param_pattern_colormode) is 0 = the texture as it is; 2 (a T-shirt's) tints by the region colours.
constexpr std::uint64_t region_a_key = 0x99879bf202d1960aULL; // param_region_color_1a
constexpr std::array<std::uint64_t, 3> stitch_keys{0x18984e85c679da8dULL, 0x2bf96fc9d8da57bbULL, 0x13cfb51bd8474129ULL}; // stitchcolor, a, c
constexpr std::uint32_t tint_mode = 2;
struct Finish { const char* name; std::array<Color, 3> stops; bool chameleon; };
constexpr std::array<Finish, 12> finishes{{
    {"Own", {}, false},
    {"purple", {{{0.42f, 0.10f, 0.85f}, {0.42f, 0.10f, 0.85f}, {0.42f, 0.10f, 0.85f}}}, false},
    {"acid green", {{{0.40f, 0.85f, 0.02f}, {0.40f, 0.85f, 0.02f}, {0.40f, 0.85f, 0.02f}}}, false},
    {"blue", {{{0.06f, 0.36f, 0.85f}, {0.06f, 0.36f, 0.85f}, {0.06f, 0.36f, 0.85f}}}, false},
    {"red", {{{0.85f, 0.06f, 0.04f}, {0.85f, 0.06f, 0.04f}, {0.85f, 0.06f, 0.04f}}}, false},
    {"gold", {{{0.85f, 0.60f, 0.20f}, {0.85f, 0.60f, 0.20f}, {0.85f, 0.60f, 0.20f}}}, false},
    {"rose gold", {{{0.85f, 0.48f, 0.40f}, {0.85f, 0.48f, 0.40f}, {0.85f, 0.48f, 0.40f}}}, false},
    {"pink", {{{0.85f, 0.18f, 0.55f}, {0.85f, 0.18f, 0.55f}, {0.85f, 0.18f, 0.55f}}}, false},
    {"black chrome", {{{0.03f, 0.03f, 0.036f}, {0.03f, 0.03f, 0.036f}, {0.03f, 0.03f, 0.036f}}}, false},
    {"chameleon", {{{0.45f, 0.08f, 0.85f}, {0.04f, 0.60f, 0.70f}, {0.35f, 0.85f, 0.06f}}}, true},
    {"oil slick", {{{0.10f, 0.20f, 0.85f}, {0.70f, 0.10f, 0.75f}, {0.85f, 0.62f, 0.10f}}}, true},
    {"sunset", {{{0.85f, 0.55f, 0.10f}, {0.85f, 0.15f, 0.45f}, {0.38f, 0.08f, 0.80f}}}, true},
}};
// fit58 (user: "colours that suit a strap - leather, camo, metal; the carabiner's ones do not always fit a strap"):
// the strap's own palette (the stitch colour of its material, linear RGB like the finishes)
constexpr std::array<Finish, 31> strap_finishes{{
    {"Own", {}, false},
    {"black", {{{0.012f, 0.012f, 0.013f}, {0.012f, 0.012f, 0.013f}, {0.012f, 0.012f, 0.013f}}}, false},
    {"charcoal", {{{0.040f, 0.040f, 0.045f}, {0.040f, 0.040f, 0.045f}, {0.040f, 0.040f, 0.045f}}}, false},
    {"grey", {{{0.20f, 0.20f, 0.21f}, {0.20f, 0.20f, 0.21f}, {0.20f, 0.20f, 0.21f}}}, false},
    {"white", {{{0.80f, 0.80f, 0.78f}, {0.80f, 0.80f, 0.78f}, {0.80f, 0.80f, 0.78f}}}, false},
    {"cream", {{{0.78f, 0.66f, 0.45f}, {0.78f, 0.66f, 0.45f}, {0.78f, 0.66f, 0.45f}}}, false},
    {"tan leather", {{{0.45f, 0.25f, 0.11f}, {0.45f, 0.25f, 0.11f}, {0.45f, 0.25f, 0.11f}}}, false},
    {"cognac leather", {{{0.50f, 0.17f, 0.045f}, {0.50f, 0.17f, 0.045f}, {0.50f, 0.17f, 0.045f}}}, false},
    {"brown leather", {{{0.20f, 0.09f, 0.035f}, {0.20f, 0.09f, 0.035f}, {0.20f, 0.09f, 0.035f}}}, false},
    {"dark brown leather", {{{0.075f, 0.033f, 0.014f}, {0.075f, 0.033f, 0.014f}, {0.075f, 0.033f, 0.014f}}}, false},
    {"olive camo", {{{0.11f, 0.13f, 0.035f}, {0.11f, 0.13f, 0.035f}, {0.11f, 0.13f, 0.035f}}}, false},
    {"forest camo", {{{0.028f, 0.085f, 0.035f}, {0.028f, 0.085f, 0.035f}, {0.028f, 0.085f, 0.035f}}}, false},
    {"desert camo", {{{0.40f, 0.32f, 0.16f}, {0.40f, 0.32f, 0.16f}, {0.40f, 0.32f, 0.16f}}}, false},
    {"coyote brown", {{{0.27f, 0.17f, 0.085f}, {0.27f, 0.17f, 0.085f}, {0.27f, 0.17f, 0.085f}}}, false},
    {"sand", {{{0.60f, 0.50f, 0.30f}, {0.60f, 0.50f, 0.30f}, {0.60f, 0.50f, 0.30f}}}, false},
    {"navy", {{{0.012f, 0.025f, 0.10f}, {0.012f, 0.025f, 0.10f}, {0.012f, 0.025f, 0.10f}}}, false},
    {"royal blue", {{{0.03f, 0.10f, 0.55f}, {0.03f, 0.10f, 0.55f}, {0.03f, 0.10f, 0.55f}}}, false},
    {"sky blue", {{{0.30f, 0.55f, 0.85f}, {0.30f, 0.55f, 0.85f}, {0.30f, 0.55f, 0.85f}}}, false},
    {"teal", {{{0.01f, 0.28f, 0.28f}, {0.01f, 0.28f, 0.28f}, {0.01f, 0.28f, 0.28f}}}, false},
    {"red", {{{0.70f, 0.03f, 0.03f}, {0.70f, 0.03f, 0.03f}, {0.70f, 0.03f, 0.03f}}}, false},
    {"burgundy", {{{0.22f, 0.015f, 0.03f}, {0.22f, 0.015f, 0.03f}, {0.22f, 0.015f, 0.03f}}}, false},
    {"orange", {{{0.85f, 0.25f, 0.02f}, {0.85f, 0.25f, 0.02f}, {0.85f, 0.25f, 0.02f}}}, false},
    {"yellow", {{{0.85f, 0.65f, 0.02f}, {0.85f, 0.65f, 0.02f}, {0.85f, 0.65f, 0.02f}}}, false},
    {"neon green", {{{0.30f, 0.85f, 0.02f}, {0.30f, 0.85f, 0.02f}, {0.30f, 0.85f, 0.02f}}}, false},
    {"purple", {{{0.25f, 0.05f, 0.60f}, {0.25f, 0.05f, 0.60f}, {0.25f, 0.05f, 0.60f}}}, false},
    {"pink", {{{0.85f, 0.30f, 0.50f}, {0.85f, 0.30f, 0.50f}, {0.85f, 0.30f, 0.50f}}}, false},
    {"hot pink", {{{0.85f, 0.06f, 0.35f}, {0.85f, 0.06f, 0.35f}, {0.85f, 0.06f, 0.35f}}}, false},
    {"silver", {{{0.55f, 0.56f, 0.58f}, {0.55f, 0.56f, 0.58f}, {0.55f, 0.56f, 0.58f}}}, false},
    {"gold", {{{0.75f, 0.52f, 0.18f}, {0.75f, 0.52f, 0.18f}, {0.75f, 0.52f, 0.18f}}}, false},
    {"gunmetal", {{{0.10f, 0.10f, 0.11f}, {0.10f, 0.10f, 0.11f}, {0.10f, 0.10f, 0.11f}}}, false},
    {"copper", {{{0.62f, 0.25f, 0.12f}, {0.62f, 0.25f, 0.12f}, {0.62f, 0.25f, 0.12f}}}, false},
}};
Color mix(const Color& a, const Color& b, float t) noexcept { return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t}; }
// A chameleon's colour follows the angle the camera sees the skater from (around and above), and drifts slowly.
Color finish_color(const Finish& f, float brightness) {
    auto& s = state();
    Color out = f.stops[0];
    if (f.chameleon) {
        const float seconds = static_cast<float>(GetTickCount64() % 3600000) / 1000.0f;
        float phase = seconds * 0.5f;
        Packed hips{};
        std::uint64_t at{};
        AcquireSRWLockShared(&s.hips_lock);
        hips = s.hips_bone, at = s.hips_at;
        ReleaseSRWLockShared(&s.hips_lock);
        const auto view = latest_game_view();
        if (view && at && GetTickCount64() - at < 500) {
            const auto centre = through(hips, {0.0f, 0.95f, 0.0f});
            std::array<float, 3> v{view->world[12] - centre[0], view->world[13] - centre[1], view->world[14] - centre[2]};
            const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (length > 0.01f) {
                for (auto& c : v) c /= length;
                const float right = v[0] * hips[0] + v[1] * hips[4] + v[2] * hips[8];
                const float up = v[0] * hips[1] + v[1] * hips[5] + v[2] * hips[9];
                const float forward = v[0] * hips[2] + v[1] * hips[6] + v[2] * hips[10];
                phase = std::atan2(right, forward) * 1.6f + std::asin(std::clamp(up, -1.0f, 1.0f)) * 2.5f + seconds * 0.25f;
            }
        }
        const float t = 0.5f + 0.5f * std::sin(phase);
        out = t < 0.5f ? mix(f.stops[0], f.stops[1], t * 2.0f) : mix(f.stops[1], f.stops[2], t * 2.0f - 1.0f);
    }
    for (auto& c : out) c = std::clamp(c * brightness, 0.002f, 1.0f);
    return out;
}
// The colour mode node of a material, found afresh (never a remembered address: a swapped item frees them).
std::uintptr_t colormode_node(std::uintptr_t material) {
    for (const auto& n : parameter_nodes(material)) {
        std::uint64_t hash{};
        std::memcpy(&hash, n.bytes.data() + 8, 8);
        if (hash == colormode_key) return n.node;
    }
    return 0;
}
// fit30: the wardrobe's preview character has an appearance component of its own (not the skater entity's): a colour
// written to the skater showed only after leaving the wardrobe (user 2026-10-19). While one of our keychains has the
// focus, a background heap scan lists the appearance components (by vtable); those besides the skater's that wear
// Accessories-slot metal of ours are the preview and get the same colour.
namespace preview {
std::mutex lock;
std::vector<std::uintptr_t> candidates, kept;
std::atomic<bool> scanning{};
std::atomic<std::uint64_t> scanned_at{};
std::uint64_t checked_at{}, kept_seen{};
constexpr std::size_t most = 1024;
std::uint64_t scan_range(const std::uint8_t* p, std::size_t n, std::uintptr_t start, std::uint64_t want, std::uintptr_t* out,
                         std::size_t& count) noexcept {
    __try {
        for (std::size_t i = 0; i + 8 <= n; i += 8)
            if (*reinterpret_cast<const std::uint64_t*>(p + i) == want && count < most) out[count++] = start + i;
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 1; }
}
void run(std::uint64_t want) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const auto began = GetTickCount64();
    std::vector<std::uintptr_t> found(most);
    std::size_t count{};
    std::uint64_t faults{};
    try {
        MEMORY_BASIC_INFORMATION info{};
        for (std::uintptr_t at = 0x10000; at < memory::highest_user_address;) {
            if (!VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info))) break;
            const auto start = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
            const auto size = static_cast<std::size_t>(info.RegionSize);
            at = start + size;
            if (info.State != MEM_COMMIT || info.Type != MEM_PRIVATE ||
                (info.Protect & (PAGE_GUARD | PAGE_NOACCESS | PAGE_NOCACHE | PAGE_WRITECOMBINE)))
                continue;
            const DWORD kind = info.Protect & 0xff;
            if (kind != PAGE_READWRITE && kind != PAGE_EXECUTE_READWRITE) continue;
            faults += scan_range(reinterpret_cast<const std::uint8_t*>(start), size, start, want, found.data(), count);
        }
    } catch (...) {}
    found.resize(count);
    {
        std::lock_guard guard(lock);
        candidates = std::move(found);
        kept.clear();
        checked_at = 0;
    }
    note(std::format("color: preview scan - {} appearance component(s) in {} ms, {} fault(s)", count, GetTickCount64() - began, faults));
    scanned_at.store(GetTickCount64());
    scanning.store(false);
}
} // namespace preview
void tick_finish(std::uintptr_t base, std::uintptr_t client) {
    auto& s = state();
    const int finish = std::clamp(s.finish.load(std::memory_order_acquire), 0, static_cast<int>(finishes.size()) - 1);
    const int strap = std::clamp(s.strap.load(std::memory_order_acquire), 0, static_cast<int>(strap_finishes.size()) - 1);
    // fit34: while the panel (or a grid) has one of ours, a walk now and then says whether it has a strap material
    const auto now0 = GetTickCount64();
    const bool look = (now0 < s.focus_until.load(std::memory_order_acquire) + 1000 ||
                       now0 < s.prepare_until.load(std::memory_order_acquire) + 1000) && now0 - s.strap_looked > 700;
    if (!finish && !s.finish_active && !strap && !s.strap_active && !look) return;
    std::array<unsigned char, addr::native_cosmetics::publish_materials_prefix.size()> prefix{};
    if (!memory::peek_bytes(base + addr::native_cosmetics::publish_materials, prefix.data(), prefix.size()) ||
        prefix != addr::native_cosmetics::publish_materials_prefix)
        return;
    const auto local = multiplayer::capture_local(base, client, false);
    if (!local.ready) return;
    auto read = [](std::uintptr_t address, void* out, std::size_t size) { return memory::peek_bytes(address, out, size); };
    const auto live = developer_hoodie_detail::materials(read, base, local.entity);
    if (live.pending) return;
    std::vector<developer_hoodie_detail::Binding> metal;
    // acc8: the stitch colours. Region colours do nothing in the full-colour mode our preset uses, but the stitch colour
    // paints wherever the rgb texture's G (stitch mask) is set, live (the user's Carhartt pants: only their stitches took
    // the colour). Items whose rgb has the stitch mask on the metal ("Keychain Finish Test") get coloured chrome.
    for (const auto& b : live.bindings)
        if (b.part == outfit_part && std::find(stitch_keys.begin(), stitch_keys.end(), b.key.hash) != stitch_keys.end()) metal.push_back(b);
    // fit27: our items are in the Accessories slot: only its metal when it has any (an outfit worn beside keeps its own)
    if (std::any_of(metal.begin(), metal.end(), [](const auto& b) { return b.accessory; }))
        std::erase_if(metal, [](const auto& b) { return !b.accessory; });
    // fit30: the same metal on the wardrobe's preview character, while one of our keychains has the focus there
    if (const auto now = GetTickCount64(); now < s.prepare_until.load(std::memory_order_acquire) + 4000) {
        const auto ours = [](const developer_hoodie_detail::Binding& b) {
            return b.accessory && std::find(stitch_keys.begin(), stitch_keys.end(), b.key.hash) != stitch_keys.end();
        };
        std::vector<std::uintptr_t> cands, kept;
        {
            std::lock_guard guard(preview::lock);
            cands = preview::candidates, kept = preview::kept;
        }
        if ((cands.empty() || (kept.empty() && now - preview::scanned_at.load() > 6000)) && now - preview::scanned_at.load() > 3000 &&
            !preview::scanning.exchange(true)) {
            try {
                std::thread(preview::run, static_cast<std::uint64_t>(base + addr::engine::skater_appearance_vtable)).detach();
            } catch (...) { preview::scanning.store(false); }
        }
        if (!cands.empty() && kept.empty() && now - preview::checked_at > 1000) {
            preview::checked_at = now;
            for (const auto a : cands) {
                if (a == live.appearance) continue;
                try {
                    const auto pm = developer_hoodie_detail::appearance_materials(read, base, a);
                    if (std::any_of(pm.bindings.begin(), pm.bindings.end(), ours)) kept.push_back(a);
                } catch (...) {}
            }
            std::lock_guard guard(preview::lock);
            preview::kept = kept;
            if (!kept.empty()) note(std::format("color: preview character(s) {} of {} appearance component(s)", kept.size(), cands.size()));
        }
        bool seen{};
        for (const auto a : kept) {
            try {
                const auto pm = developer_hoodie_detail::appearance_materials(read, base, a);
                for (const auto& b : pm.bindings)
                    if (ours(b)) metal.push_back(b), seen = true;
            } catch (...) {}
        }
        if (seen) preview::kept_seen = now;
        else if (!kept.empty() && now - preview::kept_seen > 2000) {
            std::lock_guard guard(preview::lock);   // gone (the wardrobe was left and opened again): found anew
            preview::kept.clear(), preview::candidates.clear();
        }
    }
    // fit34: the strap - our accessory's second material (Outfit_Bottom_mat, AppearanceShaderPreset2) - has its own colour
    std::vector<developer_hoodie_detail::Binding> strapb;
    const auto bottom = developer_hoodie_detail::material_hash("Outfit_Bottom_mat");
    const auto is_strap = [&](const developer_hoodie_detail::Binding& b) { return b.accessory && b.material_name == bottom; };
    for (const auto& b : metal)
        if (is_strap(b)) strapb.push_back(b);
    std::erase_if(metal, is_strap);
    if (look) s.strap_looked = now0, s.has_strap.store(!strapb.empty(), std::memory_order_release);
    if (!finish && !s.finish_active && !strap && !s.strap_active) return;
    std::erase_if(s.strap_colors, [&](const Saved& x) { return std::none_of(strapb.begin(), strapb.end(), [&](const auto& b) { return b.material == x.material; }); });
    // What was saved of a material no longer worn belongs to nothing: forgotten, never written.
    std::erase_if(s.finish_colors, [&](const Saved& x) { return std::none_of(metal.begin(), metal.end(), [&](const auto& b) { return b.material == x.material; }); });
    std::erase_if(s.finish_modes, [&](const SavedMode& x) { return std::none_of(metal.begin(), metal.end(), [&](const auto& b) { return b.material == x.material; }); });
    const auto item = !metal.empty() ? metal.front().item : !strapb.empty() ? strapb.front().item : 0;
    if (!item) return;
    bool wrote{};
    // fit29: what a pick writes, logged once per pick, and read back 0.6 s later
    static std::uint64_t check_at{};
    static int logged_pick = -1;
    const auto describe = [&](const char* what) {
        std::string line = std::format("color: {} - {} binding(s)", what, metal.size());
        for (const auto& b : metal) {
            Color now{};
            if (b.node) (void)memory::peek_bytes(b.node + 0x10, now.data(), sizeof(now));
            line += std::format("; item {:#x} material {:#x} node {:#x} key {:#x}{} now ({:.3f} {:.3f} {:.3f})", b.item, b.material,
                                b.node, b.key.hash, b.accessory ? " [accessories]" : " [outfit]", now[0], now[1], now[2]);
        }
        note(line);
    };
    if (finish != logged_pick) describe(std::format("pick {} before writing", finish).c_str()), logged_pick = finish, check_at = GetTickCount64() + 600;
    else if (check_at && GetTickCount64() >= check_at) describe("0.6 s after the pick"), check_at = 0;
    const auto set_both = [&](const developer_hoodie_detail::Binding& b, const Color& color) {
        bool ok = b.node && write_color(b.material, b.node, color);
        // and through the game's own setter (it adds / updates the parameter and marks the material)
        developer_hoodie_detail::Binding added = b;
        added.node = 0;
        ok = write_color(base, added, color) || ok;
        return ok;
    };
    if (finish) {
        const auto color = finish_color(finishes[finish], s.brightness.load());
        for (const auto& b : metal) {
            if (std::none_of(s.finish_colors.begin(), s.finish_colors.end(), [&](const Saved& x) { return x.material == b.material && x.key == b.key.hash; }))
                s.finish_colors.push_back({b.item, b.material, 0, b.key.hash, b.color});
            if (b.color != color && set_both(b, color)) wrote = true;
            if (const auto node = s.finish_mode.load() >= 0 ? colormode_node(b.material) : 0) {
                if (std::none_of(s.finish_modes.begin(), s.finish_modes.end(), [&](const SavedMode& x) { return x.material == b.material; })) {
                    SavedMode saved{b.material};
                    (void)memory::peek_bytes(node + 0x10, saved.original.data(), 16);
                    s.finish_modes.push_back(saved);
                }
                std::uint32_t now{};
                const auto mode = static_cast<std::uint32_t>(s.finish_mode.load());
                if (memory::peek(node + 0x10, now) && now != mode && write_bytes_at(node + 0x10, &mode, 4, b.material)) wrote = true;
            }
        }
        s.finish_active = true;
    } else {
        for (const auto& b : metal) {
            for (const auto& c : s.finish_colors)
                if (c.material == b.material && c.key == b.key.hash && set_both(b, c.original)) wrote = true;
            for (const auto& m : s.finish_modes)
                if (m.material == b.material)
                    if (const auto node = colormode_node(b.material); node && write_bytes_at(node + 0x10, m.original.data(), 4, b.material)) wrote = true;
        }
        s.finish_colors.clear(), s.finish_modes.clear();
        s.finish_active = false;
    }
    if (strap) {
        const auto color = finish_color(strap_finishes[strap], s.brightness.load());
        for (const auto& b : strapb) {
            if (std::none_of(s.strap_colors.begin(), s.strap_colors.end(), [&](const Saved& x) { return x.material == b.material && x.key == b.key.hash; }))
                s.strap_colors.push_back({b.item, b.material, 0, b.key.hash, b.color});
            if (b.color != color && set_both(b, color)) wrote = true;
        }
        s.strap_active = true;
    } else if (s.strap_active) {
        for (const auto& b : strapb)
            for (const auto& c : s.strap_colors)
                if (c.material == b.material && c.key == b.key.hash && set_both(b, c.original)) wrote = true;
        s.strap_colors.clear();
        s.strap_active = false;
    }
    if (strap != s.strap_logged) {
        note(std::format("strap colour {} ({}) on {} binding(s)", strap, strap_finishes[strap].name, strapb.size()));
        s.strap_logged = strap;
        s.finish_settle = settle_publishes;
    }
    if (finish != s.finish_logged) {
        note(std::format("finish {} ({}) on {} stitch colour(s), colour mode {}", finish, finishes[finish].name, metal.size(),
                         s.finish_mode.load() >= 0 ? std::to_string(s.finish_mode.load()) : std::string("untouched")));
        s.finish_logged = finish;
        s.finish_settle = settle_publishes;
    }
    // The renderer shows a colour one publish late: a colour that stopped changing is published a few more times.
    if (wrote) s.finish_settle = settle_publishes;
    if (wrote || s.finish_settle) {
        // fit29: every item of the outfit part (ours is in the Accessories slot), not only the first one
        std::vector<std::uintptr_t> items{item};
        for (const auto& b : live.bindings)
            if (b.part == outfit_part && std::find(items.begin(), items.end(), b.item) == items.end()) items.push_back(b.item);
        for (const auto& b : metal)   // fit30: the preview character's items too
            if (std::find(items.begin(), items.end(), b.item) == items.end()) items.push_back(b.item);
        for (const auto& b : strapb)
            if (std::find(items.begin(), items.end(), b.item) == items.end()) items.push_back(b.item);
        for (const auto it : items) (void)publish_materials(base, it);
        if (!wrote) --s.finish_settle;
    }
}

// acc7: which colour parameter (and colour mode) colours the metal? A timed walk through them, one at a time, with
// the step shown at the bottom of the screen; the user says which numbers made the carabiner purple.
struct TestStep { std::uint32_t mode; std::uint64_t key; const char* label; };
struct TestKey { std::uint64_t hash; std::uint16_t identity; };
constexpr std::array<TestKey, 13> test_keys{{
    {0xf526305270d94cb3ULL, 0x08e5}, {0x99879bf202d1960aULL, 0x25a4}, {0x998450425dbd9044ULL, 0x25a4}, {0x7c06880748ded394ULL, 0x564a},
    {0xfca79ddc325249ceULL, 0x1f99}, {0xfca496001ea60718ULL, 0x1f99}, {0x8c46cd354b18a9e2ULL, 0x8c54}, {0x1564a942cb33c53bULL, 0x16e6},
    {0x1567b5742f4ec0b2ULL, 0x16e6}, {0x5e1943a7c20ab77cULL, 0x1022}, {0x18984e85c679da8dULL, 0xd291}, {0x2bf96fc9d8da57bbULL, 0x24b4},
    {0x13cfb51bd8474129ULL, 0x9da0}}};
constexpr std::array<TestStep, 21> test_steps{{
    {0, 0, "own look"},
    {2, 0, "mode 2, nothing coloured"},
    {2, 0xf526305270d94cb3ULL, "mode 2 + region 1"},
    {2, 0x99879bf202d1960aULL, "mode 2 + region 1a"},
    {2, 0x998450425dbd9044ULL, "mode 2 + region 1b"},
    {2, 0x7c06880748ded394ULL, "mode 2 + region 2"},
    {2, 0xfca79ddc325249ceULL, "mode 2 + region 2a"},
    {2, 0xfca496001ea60718ULL, "mode 2 + region 2b"},
    {2, 0x8c46cd354b18a9e2ULL, "mode 2 + base colour"},
    {2, 0x1564a942cb33c53bULL, "mode 2 + colour 1564"},
    {2, 0x1567b5742f4ec0b2ULL, "mode 2 + colour 1567"},
    {2, 0x5e1943a7c20ab77cULL, "mode 2 + colour 5e19"},
    {2, 0x18984e85c679da8dULL, "mode 2 + stitch colour"},
    {2, 0x2bf96fc9d8da57bbULL, "mode 2 + stitch colour a"},
    {2, 0x13cfb51bd8474129ULL, "mode 2 + stitch colour c"},
    {1, 0x99879bf202d1960aULL, "mode 1 + region 1a"},
    {3, 0x99879bf202d1960aULL, "mode 3 + region 1a"},
    {4, 0x99879bf202d1960aULL, "mode 4 + region 1a"},
    {5, 0x99879bf202d1960aULL, "mode 5 + region 1a"},
    {1, 0xf526305270d94cb3ULL, "mode 1 + region 1"},
    {3, 0xf526305270d94cb3ULL, "mode 3 + region 1"},
}};
constexpr std::uint64_t test_step_ms = 3000;
constexpr Color test_purple{0.6f, 0.0f, 1.0f};
void tick_colortest(std::uintptr_t base, std::uintptr_t client) {
    auto& s = state();
    if (!s.test_running) return;
    const auto now = GetTickCount64();
    const auto step = static_cast<std::size_t>((now - s.test_began) / test_step_ms);
    const bool finished = step >= test_steps.size();
    std::array<unsigned char, addr::native_cosmetics::publish_materials_prefix.size()> prefix{};
    std::array<unsigned char, addr::native_cosmetics::set_shader_parameter_prefix.size()> setter{};
    if (!memory::peek_bytes(base + addr::native_cosmetics::publish_materials, prefix.data(), prefix.size()) ||
        prefix != addr::native_cosmetics::publish_materials_prefix ||
        !memory::peek_bytes(base + addr::native_cosmetics::set_shader_parameter, setter.data(), setter.size()) ||
        setter != addr::native_cosmetics::set_shader_parameter_prefix)
        return;
    const auto local = multiplayer::capture_local(base, client, false);
    if (!local.ready) return;
    auto read = [](std::uintptr_t address, void* out, std::size_t size) { return memory::peek_bytes(address, out, size); };
    const auto live = developer_hoodie_detail::materials(read, base, local.entity);
    if (live.pending) return;
    std::uint16_t type_index{};
    (void)memory::peek(base + addr::native_cosmetics::color_rgb_type + 8, type_index);
    std::vector<std::uintptr_t> materials;
    std::uintptr_t item{};
    for (const auto& b : live.bindings)
        if (b.part == outfit_part) {
            item = b.item;
            if (std::find(materials.begin(), materials.end(), b.material) == materials.end()) materials.push_back(b.material);
        }
    if (!item) return;
    const auto& current = test_steps[finished ? 0 : step];
    bool wrote{};
    for (const auto material : materials) {
        for (const auto& k : test_keys) {
            developer_hoodie_detail::Binding binding{item, material, 0, {k.identity, sizeof(Color), type_index, 1, k.hash}, developer_hoodie_detail::neutral,
                                                    static_cast<std::uint8_t>(outfit_part)};
            for (const auto& b : live.bindings)
                if (b.material == material && b.key.hash == k.hash) binding = b;
            auto saved = std::find_if(s.test_saved.begin(), s.test_saved.end(), [&](const TestSaved& x) { return x.material == material && x.key == k.hash; });
            if (saved == s.test_saved.end()) {
                s.test_saved.push_back({material, k.hash, binding.node ? binding.color : developer_hoodie_detail::neutral});
                saved = s.test_saved.end() - 1;
            }
            const auto want = !finished && current.key == k.hash ? test_purple : saved->original;
            if ((!binding.node || binding.color != want) && write_color(base, binding, want)) wrote = true;
        }
        if (const auto node = colormode_node(material)) {
            if (std::none_of(s.test_modes.begin(), s.test_modes.end(), [&](const SavedMode& x) { return x.material == material; })) {
                SavedMode saved{material};
                (void)memory::peek_bytes(node + 0x10, saved.original.data(), 16);
                s.test_modes.push_back(saved);
            }
            std::uint32_t mode = current.mode, value{};
            for (const auto& m : s.test_modes)
                if (m.material == material && finished) std::memcpy(&mode, m.original.data(), 4);
            if (memory::peek(node + 0x10, value) && value != mode && write_bytes_at(node + 0x10, &mode, 4, material)) wrote = true;
        }
    }
    if (wrote) s.test_settle = settle_publishes;
    if (wrote || s.test_settle) {
        (void)publish_materials(base, item);
        if (!wrote) --s.test_settle;
    }
    if (finished) {
        if (s.test_settle) return; // published back to the item's own look first
        s.test_running = false, s.test_saved.clear(), s.test_modes.clear();
        note("colour test: done, the item's own look restored");
        return;
    }
    if (static_cast<int>(step) != s.test_shown) {
        s.test_shown = static_cast<int>(step);
        const auto text = std::format("COLOUR TEST {} / {}: {}", step, test_steps.size() - 1, current.label);
        note(text);
    }
}

void search(bool report) {
    auto& s = state();
    std::array<int, snapshot_slots> blocks{};
    blocks.fill(-1);
    std::vector<Snapshot> shots;
    AcquireSRWLockShared(&s.lock);
    try {
        shots.assign(s.snapshots.begin(), s.snapshots.end());
    } catch (...) {}
    ReleaseSRWLockShared(&s.lock);
    // Near the skater: the skater's palette carries the skeleton (Hips = 7).
    std::array<float, 3> hips{};
    bool have_hips{};
    const auto now = GetTickCount64();
    for (const auto& shot : shots)
        if (shot.object && shot.count > 7 && now - shot.at < 3000 &&
            (shot.index == s.indices[0].load() || shot.index == s.indices[1].load())) {
            hips = through(shot.bones[7], {0.0f, 0.95f, 0.0f});
            have_hips = true;
        }
    std::string summary;
    std::size_t seen{};
    for (std::size_t i = 0; i < shots.size(); ++i) {
        const auto& shot = shots[i];
        if (!shot.object || shot.bones.size() != shot.count || now - shot.at > 3000) continue;
        ++seen;
        if (shot.count >= anchors_need) blocks[i] = find_block(shot.bones, shot.count);
        const auto at = through(shot.bones[0], {0.0f, 0.9f, 0.0f});
        const float away = have_hips ? distance(at, hips) : -1.0f;
        const bool skater = shot.index == s.indices[0].load() || shot.index == s.indices[1].load();
        if (!report && !(blocks[i] >= 0 || skater || (away >= 0 && away < 3.0f))) continue;
        summary += std::format(" [{:x} vt {:x} idx {}{} bones {} rel {} vis {} {:.2f} m{}]", shot.object & 0xffffff, shot.vtable,
                               static_cast<int>(shot.index), skater ? " SKATER" : "", shot.count, shot.relative, shot.visible, away,
                               blocks[i] >= 0 ? std::format(" OURS at {}", blocks[i]) : blocks[i] == -2 ? std::string(" several fits") : std::string());
    }
    for (std::size_t i = 0; i < snapshot_slots; ++i) {
        s.found_object[i].store(blocks[i] >= 0 ? shots[i].object : 0, std::memory_order_release);
        s.found_count[i].store(shots[i].count, std::memory_order_release);
        s.found_block[i].store(blocks[i], std::memory_order_release);
    }
    summary = std::format("{} objects drawn;{}", seen, summary);
    if (report || summary != s.last_found) {
        s.last_found = summary;
        note("palettes: " + summary);
    }
}

void write_bytes(std::ofstream& out, const void* data, std::size_t size) { out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size)); }
void dump() {
    auto& s = state();
    std::vector<Snapshot> shots;
    Raw raw;
    AcquireSRWLockShared(&s.lock);
    try {
        shots.assign(s.snapshots.begin(), s.snapshots.end());
        raw = s.raw;
    } catch (...) {}
    ReleaseSRWLockShared(&s.lock);
    SYSTEMTIME t{};
    GetLocalTime(&t);
    const auto stamp = std::format(L"{:02}{:02}{:02}", t.wHour, t.wMinute, t.wSecond);
    std::filesystem::create_directories(folder());
    std::ofstream out(folder() / (L"palette_" + stamp + L".bin"), std::ios::binary);
    std::uint32_t written{};
    for (const auto& shot : shots) {
        if (!shot.object || shot.bones.size() != shot.count) continue;
        const std::uint32_t skater = shot.index == s.indices[0].load() || shot.index == s.indices[1].load();
        const std::uint64_t object = shot.object, vtable = shot.vtable;
        const std::uint32_t relative = shot.relative | (shot.visible << 8);
        write_bytes(out, "ACC2", 4);
        write_bytes(out, &object, 8);
        write_bytes(out, &vtable, 8);
        write_bytes(out, &shot.index, 4);
        write_bytes(out, &shot.count, 4);
        write_bytes(out, &skater, 4);
        write_bytes(out, &relative, 4);
        write_bytes(out, shot.root.data(), 16);
        write_bytes(out, shot.actor.data(), 64);
        write_bytes(out, shot.bones.data(), shot.count * sizeof(Packed));
        ++written;
    }
    // The skater's packet and render object as bytes, and what their pointers point at.
    std::uint32_t targets{};
    if (raw.filled) {
        std::ofstream bytes(folder() / (L"skater_" + stamp + L".bin"), std::ios::binary);
        write_bytes(bytes, "RAW1", 4);
        write_bytes(bytes, &raw.packet, 8);
        write_bytes(bytes, raw.packet_bytes.data(), raw.packet_bytes.size());
        write_bytes(bytes, &raw.object, 8);
        write_bytes(bytes, raw.object_bytes.data(), raw.object_bytes.size());
        auto follow = [&](const std::uint8_t* data, std::size_t size) {
            for (std::size_t at = 0; at + 8 <= size; at += 8) {
                std::uint64_t value{};
                std::memcpy(&value, data + at, 8);
                if (value < 0x10000 || value > memory::highest_user_address || (value & 7)) continue;
                std::array<std::uint8_t, raw_target> target{};
                if (!memory::peek_bytes(static_cast<std::uintptr_t>(value), target.data(), target.size())) continue;
                write_bytes(bytes, "PTR1", 4);
                write_bytes(bytes, &value, 8);
                write_bytes(bytes, target.data(), target.size());
                ++targets;
            }
        };
        follow(raw.packet_bytes.data(), raw.packet_bytes.size());
        follow(raw.object_bytes.data(), raw.object_bytes.size());
    }
    note(std::format("dump: {} palettes, skater bytes {} ({} pointer targets) to {}", written, raw.filled ? "yes" : "not yet",
                     targets, folder().string()));
}

// ===================================================================================== the keychain panel's camera
// While the inventory's keychain panel is open the picture is taken from close to the keychain, from the side it hangs
// on (its place's outward direction, a little from above), whatever way the skater is turned: the rendered view's
// camera (world matrix at +0x40, rows right, up, back, position) is changed before the shaders take it, and eased
// in and out. The keychain's point: the place's top ring (KD) in bind space, 12 cm lower, through the hips' skin
// matrix the packet editor keeps (the menu skater barely moves).
struct FocusPlace { std::array<float, 3> top, out; };
constexpr std::array<FocusPlace, 14> focus_places{{
    {{0.1797f, 0.8553f, -0.0654f}, {0.0f, 0.0f, -1.0f}}, {{0.1002f, 0.8456f, 0.1538f}, {0.0f, 0.0f, 1.0f}},
    {{0.2094f, 0.8545f, -0.0002f}, {1.0f, 0.0f, 0.0f}}, {{-0.1797f, 0.8553f, -0.0654f}, {0.0f, 0.0f, -1.0f}},
    {{-0.1002f, 0.8456f, 0.1538f}, {0.0f, 0.0f, 1.0f}}, {{-0.2094f, 0.8545f, -0.0002f}, {-1.0f, 0.0f, 0.0f}},
    {{0.1002f, 0.889f, 0.1538f}, {0.0f, 0.0f, 1.0f}}, {{-0.1002f, 0.889f, 0.1538f}, {0.0f, 0.0f, 1.0f}},
    // lanyards: the strap's top (A)
    {{0.1265f, 0.9127f, -0.1209f}, {0.0f, 0.0f, -1.0f}}, {{0.1035f, 0.8808f, 0.1684f}, {0.0f, 0.0f, 1.0f}},
    {{0.2234f, 0.9060f, -0.0035f}, {1.0f, 0.0f, 0.0f}}, {{-0.1265f, 0.9127f, -0.1209f}, {0.0f, 0.0f, -1.0f}},
    {{-0.1035f, 0.8808f, 0.1684f}, {0.0f, 0.0f, 1.0f}}, {{-0.2234f, 0.9060f, -0.0035f}, {-1.0f, 0.0f, 0.0f}}}};
// The keychain in the clear part of the picture between the grid and the panel (a little right of the middle, high),
// the view 0.47 of the wardrobe's.
constexpr float focus_zoom = 0.47f, focus_height = 0.8f, focus_ndc_x = 0.15f, focus_ndc_y = 0.28f;
void normalize3(float* v) noexcept {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-6f) v[0] /= l, v[1] /= l, v[2] /= l;
}
void cross3(const float* a, const float* b, float* out) noexcept {
    out[0] = a[1] * b[2] - a[2] * b[1], out[1] = a[2] * b[0] - a[0] * b[2], out[2] = a[0] * b[1] - a[1] * b[0];
}
// The panel camera's own state, for the render threads (one lock: views may be drawn from several at once).
struct FocusCamera {
    SRWLOCK lock = SRWLOCK_INIT;
    float base_rows[12]{}, base_fov{};          // the game's camera, as it last wrote it
    float recent_rows[4][12]{}, recent_fov[4]{}; // what we wrote lately: a view still holding one of them is ours
    int recent_next{}, recent_count{};
    float last_rows[12]{}, last_fov{};          // the last camera made, kept for a frame that cannot make one
    bool have_last{}, wrote{};
    float target[3]{}, raw_last[3]{};
    bool have_target{};
    std::uintptr_t body{};                       // the skeleton aimed at, kept until it goes or another is far nearer
    LARGE_INTEGER at{};
    float blend{};
    int place{};
    // For the log, read and cleared by the client tick.
    std::uint32_t calls{}, game_new{}, ours_seen{}, kept{}, switches{}, views{};
    float jump_mm{}, dt_min{1.0f}, dt_max{};
    std::uintptr_t view_seen[4]{};
};
FocusCamera& focus_camera() noexcept {
    static FocusCamera c;
    return c;
}
void keep_last(FocusCamera& c, float* cam, float* fov, std::uintptr_t previous) noexcept {
    if (!c.have_last) return;
    std::memcpy(cam, c.last_rows, sizeof(c.last_rows));
    *fov = c.last_fov;
    if (previous) std::memcpy(reinterpret_cast<void*>(previous + 0x40), c.last_rows, sizeof(c.last_rows)), *reinterpret_cast<float*>(previous + 0x104) = c.last_fov;
    ++c.kept;
}
void make_view(FocusCamera& c, std::uintptr_t view, std::uintptr_t previous) noexcept {
    auto& s = state();
    float* cam = reinterpret_cast<float*>(view + 0x40);
    float* fov = reinterpret_cast<float*>(view + 0x104);
    ++c.calls;
    {
        bool seen = false;
        for (const auto v : c.view_seen) seen = seen || v == view;
        if (!seen) c.view_seen[c.views++ % 4] = view;
    }
    // The game's own camera: a view that holds none of what we wrote lately has the game's new one (the wardrobe
    // does not write its view every frame, and views may take turns).
    bool ours = false;
    for (int i = 0; i < c.recent_count; ++i)
        if (std::memcmp(cam, c.recent_rows[i], sizeof(c.recent_rows[i])) == 0 && *fov == c.recent_fov[i]) ours = true;
    if (ours) ++c.ours_seen;
    else std::memcpy(c.base_rows, cam, sizeof(c.base_rows)), c.base_fov = *fov, ++c.game_new;
    // Real time (the tick count steps in 16 ms: the zoom went in jumps).
    LARGE_INTEGER now_qpc{}, frequency{};
    QueryPerformanceCounter(&now_qpc);
    QueryPerformanceFrequency(&frequency);
    const float dt = c.at.QuadPart ? std::min(0.1f, static_cast<float>(now_qpc.QuadPart - c.at.QuadPart) / static_cast<float>(frequency.QuadPart)) : 0.0f;
    c.at = now_qpc;
    if (dt > 0.0f) c.dt_min = std::min(c.dt_min, dt), c.dt_max = std::max(c.dt_max, dt);
    const auto now = GetTickCount64();
    const int place = s.focus_place.load(std::memory_order_acquire);
    // fit17: the lanyards' places too (fit16 stopped at the 8 keychain places: no zoom on a lanyard)
    const bool want = now < s.focus_until.load(std::memory_order_acquire) && place >= 0 &&
                      place < static_cast<int>(focus_places.size());
    c.blend = std::clamp(c.blend + (want ? dt : -dt) * 3.0f, 0.0f, 1.0f);
    if (s.fov_seen_count < 6) s.fov_seen[s.fov_seen_count++] = c.base_fov;
    if (c.blend <= 0.0f) {
        if (c.wrote) {
            // Faded out: the game's camera back, as it had it.
            std::memcpy(cam, c.base_rows, sizeof(c.base_rows));
            *fov = c.base_fov;
            if (previous) std::memcpy(reinterpret_cast<void*>(previous + 0x40), c.base_rows, sizeof(c.base_rows)), *reinterpret_cast<float*>(previous + 0x104) = c.base_fov;
        }
        c.wrote = c.have_last = c.have_target = false;
        c.recent_count = 0;
        c.body = 0;
        return;
    }
    if (!std::isfinite(c.base_fov) || c.base_fov <= 0.05f || c.base_fov >= 3.0f) return keep_last(c, cam, fov, previous);
    // The skeleton on show: the one aimed at while it is drawn, else the nearest (a nearer one has to be a metre
    // nearer to take over: no flipping between two).
    Packed h{};
    bool found = false;
    {
        float best = 8.0f, kept_d = 1e9f;
        std::uintptr_t best_object{};
        Packed best_hips{}, kept_hips{};
        AcquireSRWLockShared(&s.hips_lock);
        for (const auto& body : s.bodies) {
            if (!body.at || now - body.at > 500) continue;
            const auto p = through(body.hips, {0.0f, 0.95f, 0.0f});
            const float dx = p[0] - cam[12], dy = p[1] - cam[13], dz = p[2] - cam[14];
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (body.object == c.body) kept_d = d, kept_hips = body.hips;
            if (d < best) best = d, best_object = body.object, best_hips = body.hips;
        }
        ReleaseSRWLockShared(&s.hips_lock);
        if (kept_d < 8.0f && !(best_object && best_object != c.body && best + 1.0f < kept_d)) h = kept_hips, found = true;
        else if (best_object) {
            if (c.body && best_object != c.body) ++c.switches;
            c.body = best_object, h = best_hips, found = true, c.have_target = false;
        }
    }
    if (!found) return keep_last(c, cam, fov, previous);
    if (want) c.place = place;
    const auto& fp = focus_places[static_cast<std::size_t>(c.place)];
    {   // fit52: how far the skater would have to turn for the keychain's side to face the camera (the panel turns the
        // wardrobe skater with the right stick until it does - fit51's camera going round showed the backdrop's dark
        // back and came too close)
        constexpr float pi = 3.14159265f;
        const auto top_w = through(h, fp.top);
        const auto tip_w = through(h, {fp.top[0] + fp.out[0], fp.top[1] + fp.out[1], fp.top[2] + fp.out[2]});
        float e = std::atan2(cam[12] - top_w[0], cam[14] - top_w[2]) - std::atan2(tip_w[0] - top_w[0], tip_w[2] - top_w[2]);
        while (e > pi) e -= 2.0f * pi;
        while (e < -pi) e += 2.0f * pi;
        if (std::isfinite(e)) s.turn_error.store(e, std::memory_order_release), s.turn_error_at.store(now, std::memory_order_release);
    }
    // Aimed between the hips and the keychain, a little lower than the hips (where the keychains hang); smoothed
    // (frame-rate free), so the camera does not follow the preview skater's idle sway.
    const std::array<float, 3> point{fp.top[0] * 0.5f, focus_height, fp.top[2] * 0.5f};
    const auto aim_point = through(h, point);
    if (c.have_target) {
        const float jx = aim_point[0] - c.raw_last[0], jy = aim_point[1] - c.raw_last[1], jz = aim_point[2] - c.raw_last[2];
        c.jump_mm = std::max(c.jump_mm, std::sqrt(jx * jx + jy * jy + jz * jz) * 1000.0f);
    }
    const float follow = 1.0f - std::exp(-dt * 3.0f);
    for (int k = 0; k < 3; ++k) {
        c.target[k] = c.have_target ? c.target[k] + (aim_point[k] - c.target[k]) * follow : aim_point[k];
        c.raw_last[k] = aim_point[k];
    }
    c.have_target = true;
    const float t = c.blend * c.blend * (3.0f - 2.0f * c.blend);
    const float aspect = std::clamp(s.focus_aspect.load(std::memory_order_acquire), 1.0f, 3.5f);
    const float* base_rows = c.base_rows;
    // The camera's own handedness, kept: right x up is +back or -back.
    float ru[3]{};
    cross3(base_rows, base_rows + 4, ru);
    const float hand = ru[0] * base_rows[8] + ru[1] * base_rows[9] + ru[2] * base_rows[10] < 0 ? -1.0f : 1.0f;
    const float v[3]{c.target[0] - cam[12], c.target[1] - cam[13], c.target[2] - cam[14]};
    // Where the target is in the game's picture, and where it goes (eased in from where it was).
    const float base_tan = std::tan(c.base_fov * 0.5f);
    const float vz0 = -(v[0] * base_rows[8] + v[1] * base_rows[9] + v[2] * base_rows[10]);
    if (vz0 <= 0.05f) return keep_last(c, cam, fov, previous);
    const float ndc0_x = (v[0] * base_rows[0] + v[1] * base_rows[1] + v[2] * base_rows[2]) / vz0 / (base_tan * aspect);
    const float ndc0_y = (v[0] * base_rows[4] + v[1] * base_rows[5] + v[2] * base_rows[6]) / vz0 / base_tan;
    const float zoom = 1.0f + (focus_zoom - 1.0f) * t;
    const float tan_v = base_tan * zoom, tan_h = tan_v * aspect;
    const float ndc_x = ndc0_x + (focus_ndc_x - ndc0_x) * t, ndc_y = ndc0_y + (focus_ndc_y - ndc0_y) * t;
    const float want_x = std::atan(ndc_x * tan_h), want_y = std::atan2(ndc_y * tan_v, std::sqrt(1.0f + ndc_x * tan_h * ndc_x * tan_h));
    // The forward that shows the target there: from straight at it, turned a few times by what is left.
    float forward[3]{v[0], v[1], v[2]};
    normalize3(forward);
    const float world_up[3]{0.0f, 1.0f, 0.0f};
    float right[3]{}, up[3]{}, back[3]{};
    for (int pass = 0; pass < 6; ++pass) {
        back[0] = -forward[0], back[1] = -forward[1], back[2] = -forward[2];
        cross3(world_up, back, right);
        normalize3(right);
        for (auto& e : right) e *= hand;
        cross3(back, right, up);
        for (auto& e : up) e *= hand;
        normalize3(up);
        if (pass == 5) break;
        const float x = v[0] * right[0] + v[1] * right[1] + v[2] * right[2];
        const float y = v[0] * up[0] + v[1] * up[1] + v[2] * up[2];
        const float z = v[0] * forward[0] + v[1] * forward[1] + v[2] * forward[2];
        const float ax = std::atan2(x, z), ay = std::atan2(y, std::sqrt(x * x + z * z));
        const float ex = std::tan(ax - want_x), ey = std::tan(ay - want_y);
        for (int k = 0; k < 3; ++k) forward[k] += right[k] * ex + up[k] * ey;
        normalize3(forward);
    }
    float rows[12]{};
    std::memcpy(rows, cam, sizeof(rows));
    for (int k = 0; k < 3; ++k) rows[k] = right[k], rows[4 + k] = up[k], rows[8 + k] = back[k];
    for (const auto e : rows)
        if (!std::isfinite(e)) return keep_last(c, cam, fov, previous);
    std::memcpy(cam, rows, sizeof(rows));
    *fov = 2.0f * std::atan(tan_v);
    // The previous view as this one: no camera motion between them (no smear from the change).
    if (previous) std::memcpy(reinterpret_cast<void*>(previous + 0x40), rows, sizeof(rows)), *reinterpret_cast<float*>(previous + 0x104) = *fov;
    std::memcpy(c.last_rows, rows, sizeof(rows));
    c.last_fov = *fov;
    c.have_last = c.wrote = true;
    std::memcpy(c.recent_rows[c.recent_next], rows, sizeof(rows));
    c.recent_fov[c.recent_next] = *fov;
    c.recent_next = (c.recent_next + 1) % 4;
    c.recent_count = std::min(c.recent_count + 1, 4);
}
// Render threads. No C++ objects with destructors here (SEH).
void edit_view(std::uintptr_t view, std::uintptr_t previous) noexcept {
    auto& c = focus_camera();
    __try {
        if (*reinterpret_cast<std::uint32_t*>(view - 0x38) != 0) return; // the main picture only
        AcquireSRWLockExclusive(&c.lock);
        __try {
            make_view(c, view, previous);
        } __finally {
            ReleaseSRWLockExclusive(&c.lock);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ===================================================================================== the keychain fit (customizer v1)
// Where a keychain hangs is the rest offset of its static top (the braided keychains' KD, the chain keychains' Q8 / Q5)
// from its thigh bone: the local Pose translation of that render bone (bone record +112 in our renderbones EBX, exact
// float bits). The game keeps it in the loaded asset, a native copy and per-instance copies; moved there, the keychain
// moves with its physics (acc4, user-confirmed). Each of the 8 places has its own offset (up, out = away from the leg,
// along = towards the side, or forward at the side), per pants (profile "Accessories.Fit.<pants>", else
// "Accessories.Fit"). Table: keychain\v5\customizer\anchor_table.py from the 1.3.1 build (KC33).
// Safety (acc6 crashed the game: freed copies re-used by the pants were written): a hit is written only while it
// holds exactly this anchor's rest value or the value we applied, otherwise it is dead for good; the patterns live
// in the DLL image (never scanned), hits are 16-byte aligned and outside the client thread's stack. Scans run only
// when an offset is in use: the first time, after a change (new instance copies carry the shifted values), and when
// the long-lived copies (asset / native, recognised by their record stride) went away.
constexpr std::size_t place_count = 14;   // 8 keychain places, then 6 lanyard places
constexpr std::array<const char*, place_count> place_keys{"back", "front", "side", "back_r", "front_r", "side_r", "front_nb",
                                                          "front_nb_r", "ly_back", "ly_front", "ly_side", "ly_back_r",
                                                          "ly_front_r", "ly_side_r"};
constexpr std::array<const char*, place_count> place_names{"Back", "Front", "Side", "Back right", "Front right", "Side right",
                                                           "Front clip", "Front clip right", "Back", "Front", "Side",
                                                           "Back right", "Front right", "Side right"};
constexpr std::size_t first_lanyard_place = 8;
struct RestAnchor {
    const char* name;
    std::uint16_t index; // in our renderbones asset
    std::uint8_t place;
    std::array<std::uint8_t, 12> local;
    std::array<float, 3> up, out, along; // 1 cm in model space, in the parent's frame
    std::array<std::uint8_t, 36> rot;    // the local Pose rotation rows (record +64 / +80 / +96, 3 floats each)
    // Unit, in the parent's frame. fit17: axis_up is the HANGING LINE, the turn's axis (to the braid's / chain's /
    // strap's bone below), not the vertical.
    std::array<float, 3> axis_up, axis_along, axis_out;
    std::array<float, 3> pivot; // the turn's pivot in the parent's frame: the bone's own origin, or the bone it hangs
                                // on (a lanyard's strap top, a Front Clip's hook)
};
// Generated by keychain\v5\customizer\anchor_table2.py from the mod build (keychain tops KD / Q8 / Q5, lanyard drivers).
#include "accessory_anchors.inc"
// fit33: the thigh capsules our accessories hang against (keychain\v6\collision\mark_capsules.py marks their radii, so
// the game's own pants capsules - the same EA values - never match). Found by the same scan as the anchors, as the
// 12 bytes (radius, usage, length) of the collider input record; a hit's anchor is cap_flag | the capsule's index.
#include "accessory_capsules.inc"
constexpr std::uint16_t cap_flag = 0x8000;
// fit36-47: the charm slots test (keychain\v7\slots, NOTES "KC40" .. "KCfast v11"). fit48 (v12, user: "Place /
// Strap do not switch; the charms still do not swing, all in one plane"): the slot item at every place, a ring + its
// charm one piece V<s> swinging from the ring's contact R<s> on the carabiner's bar, every option H<s>_<o> rigid on
// its piece (shown, or 200 m away). Per place a table (charm_table.py from the mod build); the DLL works on the place
// of the slot item worn. A hit's charm id: an option (slot x options + option), then the 3 pieces, then the 3 contacts.
#include "accessory_charms.inc"
constexpr std::uint16_t charm_flag = 0x4000;
using Fit = std::array<Offset, place_count>;
constexpr std::size_t most_hits = 4096; // fit35: 512 filled up with KC39's 530 items (the worn one's copies left out)
struct Hit { std::uintptr_t address{}; std::uint16_t anchor{}; DWORD protect{}, type{}; bool dead{}, lasting{}, unturnable{}; };
struct Pattern { std::array<std::uint8_t, 12> bytes{}; std::uint16_t anchor{}; };
// fit55: then one id per braided model's carabiner (CB_<model>, shown for the worn model)
// fit59 (KC43, user: "optimise"): ONE option H_<o>, piece V_<o> and contact R_<o> per option (charm_per_option tables),
// whichever slot it is picked for - an id is an option, then the options' pieces, then their contacts, then carabiners
static_assert(charm_per_option, "accessory_charms.inc from a KC43 (per option) build");
constexpr std::size_t charm_n = charm_option_keys.size(), charm_piece0 = charm_n, charm_ring0 = 2 * charm_n,
                      charm_cara0 = 3 * charm_n, charm_ids = charm_cara0 + charm_models.size();
std::array<Pattern, (rest_anchors.size() + rest_capsules.size()) * 2 + charm_ids * 3 + 3> scan_patterns{}; // static: in the image, so never found by the scan itself
std::size_t charm_place_cur{};   // fit48: charm_place_tables[] of the slot item worn (its place)
int charm_model_cur = -1;        // fit55: charm_models[] of the slot item worn (-1: the test item = Carabiner Camo's)
// fit53: read / saved on the client thread (tick_fit), used by the panel (present thread)
std::atomic<int> panel_turn_sign_value{0};
std::atomic<bool> panel_turn_sign_read{}, panel_turn_sign_unsaved{};
// fit58: KC41d - every braided model its own renderbones asset, so its own table set (the test item / camo: the first)
const CharmPlace& cpt() noexcept {
    return charm_model_tables[charm_model_cur > 0 ? static_cast<std::size_t>(charm_model_cur) : 0][charm_place_cur];
}
// a charm id's rest (its place in the slot mesh) and its "away" value (200 m off)
const std::array<std::uint8_t, 12>& charm_rest_t(const CharmPlace& t, std::size_t k) noexcept {
    if (k < charm_piece0) return t.opts[k].shown;
    if (k < charm_ring0) return t.pieces[k - charm_piece0].base;
    if (k < charm_cara0) return t.rings[k - charm_ring0].base;
    return t.caras[k - charm_cara0].shown;
}
const std::array<std::uint8_t, 12>* charm_away_t(const CharmPlace& t, std::size_t k) noexcept {
    if (k < charm_piece0) return &t.opts[k].hidden;
    if (k < charm_ring0) return &t.pieces[k - charm_piece0].hidden;
    if (k < charm_cara0) return &t.rings[k - charm_ring0].hidden;   // fit59: the asset holds every contact away
    return &t.caras[k - charm_cara0].hidden;
}
bool charm_plain(const CharmPlace& t, std::size_t k, const std::array<std::uint8_t, 12>& v) noexcept {
    const auto* away = charm_away_t(t, k);
    return v == charm_rest_t(t, k) || (away && v == *away);
}
// what each id's copies are to hold now (and before); pieces / contacts their rows too
std::array<std::array<std::uint8_t, 12>, charm_ids> charm_want{}, charm_was{};
bool charm_want_set{};
std::array<std::array<float, 9>, charm_ids> charm_want_rows{}, charm_was_rows{};
// fit46: every translation / rows written this session per id: a copy holding any of them is ours
constexpr std::size_t charm_history = 24;
std::array<std::array<std::array<std::uint8_t, 12>, charm_history>, charm_ids> charm_seen_t{};
std::array<std::array<std::array<float, 9>, charm_history>, charm_ids> charm_seen_r{};
std::array<std::size_t, charm_ids> charm_seen_tn{}, charm_seen_rn{};
// fit47: a bumper input record (build_combo v11: a charm-against-charm capsule on a slot's ring) = both flags
constexpr std::uint16_t bump_flag = cap_flag | charm_flag;
bool is_cap(const Hit& hit) noexcept { return (hit.anchor & bump_flag) == cap_flag; }
bool is_charm(const Hit& hit) noexcept { return (hit.anchor & bump_flag) == charm_flag; }
bool is_bump(const Hit& hit) noexcept { return (hit.anchor & bump_flag) == bump_flag; }
// A capsule's radius with the thickness (cm) added; 0 = the rest bits exactly.
std::uint32_t cap_radius(const RestCapsule& c, float coll) noexcept {
    if (coll == 0.0f) return c.radius;
    float r{};
    std::memcpy(&r, &c.radius, 4);
    r = std::max(0.01f, r + coll / 100.0f);
    std::uint32_t bits{};
    std::memcpy(&bits, &r, 4);
    return bits;
}
std::array<std::uint8_t, 12> cap_bytes(const RestCapsule& c, float coll) noexcept {
    std::array<std::uint8_t, 12> out{};
    const std::uint32_t v[3]{cap_radius(c, coll), c.usage, c.length};
    std::memcpy(out.data(), v, 12);
    return out;
}
std::size_t scan_pattern_count{};
struct Region { std::uintptr_t base{}; std::size_t size{}; DWORD protect{}, type{}; };
// Scans (fit10): a full one reads every heap region, in a few threads, and replaces the hits; a quick one reads only
// the regions that held hits ("hot") and the regions new since the last scan, and adds what it finds. fit9: a keychain
// picked in a grid was loaded for the preview after the scan had begun, so its copies waited for the next full scan,
// 1.5 s after the first move. Now: a full scan when the game is ready, quick ones when a keychain is picked (and a
// few times after), every second while one is picked or the panel is open, and right after every change.
struct RestScan {
    std::atomic<int> phase{}; // 0 never, 1 running, 2 done
    bool quick{};
    std::array<Hit, most_hits> found{};    // the scan's
    std::atomic<std::size_t> found_count{};
    std::atomic<std::uint64_t> bytes{}, faults{};
    std::uint64_t regions{}, ms{};
    std::array<bool, 65536> first{}; // low 16 bits of each pattern's first word
    std::uintptr_t stack_low{}, stack_high{};
    std::vector<Region> seen, hot;    // sorted by base: the regions the last scan saw, and those that held hits
    std::uint32_t scans{};
    // client thread
    Fit scan_applied{};               // the applied fit the scan's patterns were made from
    float scan_coll{};                // fit33: the same for the capsules' thickness
    std::array<Hit, most_hits> hits{};
    std::atomic<std::size_t> count{};
    Fit applied{};                    // what the live hits hold (cm)
    float coll_applied{};             // fit33: the thickness the live capsule hits hold (cm)
    std::array<int, 3> charms_applied{charm_defaults}; // fit36: the charm picks the live charm hits hold
    std::size_t caps_shown{};         // fit33: capsule hits written to the log with what is around them (research)
    std::uint64_t applied_at{}, scan_at{}, full_at{};
    std::uint32_t reported{};
};
RestScan& rest() { static auto* value = new RestScan; return *value; }
// The turn (fit17): about the accessory's hanging line through its pivot, in the parent's frame (row vectors:
// v' = (v - pivot) x Q + pivot). The bone below on that line (the braid's KB, the chain's A8, the strap's A) keeps its
// rest, so the physics under it keeps where gravity holds it: fit16 turned the tops about the vertical and the
// braid tore off its top ring (the braid bone's rest moved out of its box). No turn: the rest rows / translation, bit
// for bit.
using Rows = std::array<float, 9>;
Rows rest_rows(const RestAnchor& a) noexcept {
    Rows r{};
    std::memcpy(r.data(), a.rot.data(), sizeof(r));
    return r;
}
Rows axis_rows(const std::array<float, 3>& u, float degrees) noexcept {
    const float t = degrees * 3.14159265f / 180.0f, c = std::cos(t), s = std::sin(t), k = 1.0f - c;
    // column-vector Rodrigues, transposed for row vectors
    const float m[9]{c + u[0] * u[0] * k, u[0] * u[1] * k - u[2] * s, u[0] * u[2] * k + u[1] * s,
                     u[1] * u[0] * k + u[2] * s, c + u[1] * u[1] * k, u[1] * u[2] * k - u[0] * s,
                     u[2] * u[0] * k - u[1] * s, u[2] * u[1] * k + u[0] * s, c + u[2] * u[2] * k};
    Rows rt{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) rt[i * 3 + j] = m[j * 3 + i];
    return rt;
}
Rows mul_rows(const Rows& x, const Rows& y) noexcept {
    Rows z{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) z[i * 3 + j] = x[i * 3] * y[j] + x[i * 3 + 1] * y[3 + j] + x[i * 3 + 2] * y[6 + j];
    return z;
}
// fit24: the turn about the hanging line, then the tilt about `along` (out from the leg / the body), both through the
// pivot. A leaned top bends KC37's braid (blended over its length) and the lanyards' free pendulums.
Rows turn_rows(const RestAnchor& a, const Offset& o) noexcept {
    return mul_rows(axis_rows(a.axis_up, o.turn), axis_rows(a.axis_along, o.tilt));
}
std::array<float, 3> shifted(const RestAnchor& a, const Offset& o) noexcept {
    std::array<float, 3> v{};
    std::memcpy(v.data(), a.local.data(), 12);
    if (o.turn != 0.0f || o.tilt != 0.0f) {
        const auto q = turn_rows(a, o);
        const std::array<float, 3> d{v[0] - a.pivot[0], v[1] - a.pivot[1], v[2] - a.pivot[2]};
        // a top that is its own pivot does not move (its translation stays the rest's, bit for bit)
        if (d[0] != 0.0f || d[1] != 0.0f || d[2] != 0.0f)
            for (std::size_t j = 0; j < 3; ++j) v[j] = d[0] * q[j] + d[1] * q[3 + j] + d[2] * q[6 + j] + a.pivot[j];
    }
    if (o.up == 0.0f && o.out == 0.0f && o.along == 0.0f) return v;
    for (std::size_t k = 0; k < 3; ++k) v[k] += a.up[k] * o.up + a.out[k] * o.out + a.along[k] * o.along;
    return v;
}
bool fit_used(const Fit& f) noexcept { return std::any_of(f.begin(), f.end(), [](const Offset& o) { return !(o == Offset{}); }); }
Rows turned(const RestAnchor& a, const Offset& o) noexcept {
    const auto r0 = rest_rows(a);
    if (o.turn == 0.0f && o.tilt == 0.0f) return r0;
    return mul_rows(r0, turn_rows(a, o));
}
// The rotation rows in front of a translation hit (row 0 at -48, row 1 at -32, row 2 at -16, a 4th float after each),
// read; none where the memory is not there.
std::optional<Rows> read_rows(std::uintptr_t translation) noexcept {
    std::array<float, 12> raw{};
    if (!memory::peek_bytes(translation - 48, raw.data(), sizeof(raw))) return std::nullopt;
    return Rows{raw[0], raw[1], raw[2], raw[4], raw[5], raw[6], raw[8], raw[9], raw[10]};
}
bool write_rows(std::uintptr_t translation, const Rows& rows) noexcept {
    __try {
        for (int r = 0; r < 3; ++r) std::memcpy(reinterpret_cast<void*>(translation - 48 + 16 * r), rows.data() + 3 * r, 12);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// One readable range; a fault ends it early (the memory went away). No C++ objects here (SEH).
std::uint64_t scan_range(const std::uint8_t* p, std::size_t n, std::uintptr_t start, DWORD protect, DWORD type) noexcept {
    auto& r = rest();
    __try {
        for (std::size_t i = 0; i + 12 <= n; i += 16) {
            const std::uint32_t word = *reinterpret_cast<const std::uint32_t*>(p + i);
            if (!r.first[word & 0xffff]) continue;
            const auto at = start + i;
            if (at >= r.stack_low && at < r.stack_high) continue;
            for (std::size_t k = 0; k < scan_pattern_count; ++k)
                if (std::memcmp(p + i, scan_patterns[k].bytes.data(), 12) == 0) {
                    const auto slot = r.found_count.fetch_add(1);
                    if (slot < most_hits) r.found[slot] = {at, scan_patterns[k].anchor, protect, type, false, false};
                    break;
                }
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 1; }
}
const Region* region_at(const std::vector<Region>& list, std::uintptr_t address) noexcept {
    auto it = std::upper_bound(list.begin(), list.end(), address, [](std::uintptr_t a, const Region& g) { return a < g.base; });
    if (it == list.begin()) return nullptr;
    --it;
    return address < it->base + it->size ? &*it : nullptr;
}
bool same_region(const std::vector<Region>& list, const Region& g) noexcept {
    const auto* at = region_at(list, g.base);
    return at && at->base == g.base && at->size == g.size;
}
void scan_regions(const Region* list, std::size_t n) noexcept {
    auto& r = rest();
    std::uint64_t bytes{}, faults{};
    for (std::size_t i = 0; i < n; ++i) {
        faults += scan_range(reinterpret_cast<const std::uint8_t*>(list[i].base), list[i].size, list[i].base, list[i].protect, list[i].type);
        bytes += list[i].size;
    }
    r.bytes += bytes, r.faults += faults;
}
void run_scan() noexcept {
    auto& r = rest();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const auto began = GetTickCount64();
    r.first.fill(false);
    for (std::size_t k = 0; k < scan_pattern_count; ++k) {
        std::uint32_t word{};
        std::memcpy(&word, scan_patterns[k].bytes.data(), 4);
        r.first[word & 0xffff] = true;
    }
    try {
        std::vector<Region> all, list;
        all.reserve(16384);
        MEMORY_BASIC_INFORMATION info{};
        for (std::uintptr_t at = 0x10000; at < memory::highest_user_address;) {
            if (!VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info))) break;
            const auto start = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
            const auto size = static_cast<std::size_t>(info.RegionSize);
            at = start + size;
            // Heap only: never uncached or write-combined pages (GPU memory reads at a few MB/s: acc3's scan never ended).
            if (info.State != MEM_COMMIT || info.Type != MEM_PRIVATE ||
                (info.Protect & (PAGE_GUARD | PAGE_NOACCESS | PAGE_NOCACHE | PAGE_WRITECOMBINE)))
                continue;
            const DWORD kind = info.Protect & 0xff;
            if (kind != PAGE_READWRITE && kind != PAGE_EXECUTE_READWRITE) continue;
            all.push_back({start, size, info.Protect, info.Type});
        }
        list.reserve(all.size());
        for (const auto& g : all)
            if (!r.quick || region_at(r.hot, g.base) || !same_region(r.seen, g)) list.push_back(g);
        r.regions = list.size();
        // Threads, each a share of the bytes (one took 0.8 s for a full scan of ~7 GB).
        std::uint64_t total{};
        for (const auto& g : list) total += g.size;
        const unsigned threads = r.quick && total < (std::uint64_t{512} << 20) ? 1u : std::clamp(std::thread::hardware_concurrency() / 4, 2u, 4u);
        std::vector<std::thread> workers;
        workers.reserve(threads);
        std::size_t from = 0;
        for (unsigned t = 0; t < threads; ++t) {
            std::size_t to = from;
            std::uint64_t share{};
            const std::uint64_t target = total / threads;
            while (to < list.size() && (share < target || t + 1 == threads)) share += list[to++].size;
            bool started = false;
            if (t + 1 < threads) {
                try {
                    workers.emplace_back([&list, from, to] {
                        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                        scan_regions(list.data() + from, to - from);
                    });
                    started = true;
                } catch (...) {}
            }
            if (!started) scan_regions(list.data() + from, to - from);
            from = to;
        }
        for (auto& worker : workers) worker.join();
        // The regions holding hits now (a quick scan keeps the ones before while they are there).
        std::vector<Region> hot;
        if (r.quick)
            for (const auto& g : r.hot)
                if (same_region(all, g)) hot.push_back(g);
        const auto n = std::min(r.found_count.load(), most_hits);
        for (std::size_t i = 0; i < n; ++i)
            if (const auto* g = region_at(all, r.found[i].address)) hot.push_back(*g);
        std::sort(hot.begin(), hot.end(), [](const Region& a, const Region& b) { return a.base < b.base; });
        hot.erase(std::unique(hot.begin(), hot.end(), [](const Region& a, const Region& b) { return a.base == b.base; }), hot.end());
        r.hot = std::move(hot);
        r.seen = std::move(all);
    } catch (...) {}
    r.ms = GetTickCount64() - began;
    r.phase.store(2, std::memory_order_release);
}
// Client thread: a scan for the rest offsets and the values the applied fit gave them.
bool start_scan(const char* why, bool quick = false) noexcept {
    auto& r = rest();
    int idle = 0, done = 2;
    if (!r.phase.compare_exchange_strong(idle, 1) && !r.phase.compare_exchange_strong(done, 1)) return false;
    if (r.seen.empty()) quick = false; // nothing known yet
    r.quick = quick;
    r.found_count.store(0), r.bytes = 0, r.regions = 0, r.faults = 0, r.ms = 0, ++r.scans;
    r.scan_at = GetTickCount64();
    if (!quick) r.full_at = r.scan_at;
    r.scan_applied = r.applied, r.scan_coll = r.coll_applied;
    scan_pattern_count = 0;
    for (std::uint16_t a = 0; a < rest_anchors.size(); ++a) {
        scan_patterns[scan_pattern_count++] = {rest_anchors[a].local, a};
        const auto& o = r.applied[rest_anchors[a].place];
        if (!(o == Offset{})) {
            const auto v = shifted(rest_anchors[a], o);
            Pattern moved{{}, a};
            std::memcpy(moved.bytes.data(), v.data(), 12);
            scan_patterns[scan_pattern_count++] = moved;
        }
    }
    for (std::uint16_t c = 0; c < rest_capsules.size(); ++c) {
        const auto id = static_cast<std::uint16_t>(cap_flag | c);
        scan_patterns[scan_pattern_count++] = {cap_bytes(rest_capsules[c], 0.0f), id};
        if (r.coll_applied != 0.0f) scan_patterns[scan_pattern_count++] = {cap_bytes(rest_capsules[c], r.coll_applied), id};
    }
    {   // fit48: the worn place's options (shown / away), pieces (rest / away), contacts, what is wanted now, bumpers
        const auto& t = cpt();
        for (std::uint16_t c = 0; c < charm_ids; ++c) {
            const auto id = static_cast<std::uint16_t>(charm_flag | c);
            scan_patterns[scan_pattern_count++] = {charm_rest_t(t, c), id};
            if (const auto* away = charm_away_t(t, c)) scan_patterns[scan_pattern_count++] = {*away, id};
            if (charm_want_set && !charm_plain(t, c, charm_want[c])) scan_patterns[scan_pattern_count++] = {charm_want[c], id};
        }
        for (std::uint16_t c = 0; c < t.bumpers.size(); ++c)   // fit50: length 0 = none built (v13)
            if (t.bumpers[c].length) scan_patterns[scan_pattern_count++] = {t.bumpers[c].offset, static_cast<std::uint16_t>(bump_flag | c)};
    }
    ULONG_PTR low{}, high{};
    GetCurrentThreadStackLimits(&low, &high);
    r.stack_low = low, r.stack_high = high;
    if (!quick) note(std::format("fit: scan {} started ({}), {} patterns", r.scans, why, scan_pattern_count));
    try {
        std::thread([] { run_scan(); }).detach();
        return true;
    } catch (...) { r.phase.store(0); return false; }
}
// Client thread, a scan done: a full one's hits replace the ones before, a quick one's new ones are added. The
// long-lived copies: a whole bone array (the asset's 144-byte records, the native copy's 160-byte ones) where most
// anchors sit at their index's place. Instance copies come and go; these stay while the asset is loaded.
std::size_t take_scan(RestScan& r) {
    const auto n = std::min(r.found_count.load(), most_hits);
    std::size_t added{};
    if (!r.quick) {
        std::copy_n(r.found.begin(), n, r.hits.begin());
        r.count.store(n);
        added = n;
    } else {
        std::size_t live{};
        const auto count = std::min(r.count.load(), most_hits);
        for (std::size_t i = 0; i < count; ++i)
            if (!r.hits[i].dead) r.hits[live++] = r.hits[i];
        std::unordered_set<std::uintptr_t> known;   // fit35: a set (the list is 8x longer now)
        known.reserve(live * 2 + 16);
        for (std::size_t j = 0; j < live; ++j) known.insert(r.hits[j].address);
        for (std::size_t i = 0; i < n && live < most_hits; ++i)
            if (known.insert(r.found[i].address).second) r.hits[live++] = r.found[i], ++added;
        r.count.store(live);
    }
    const auto count = std::min(r.count.load(), most_hits);
    for (std::size_t i = 0; i < count; ++i) r.hits[i].lasting = false;
    for (const std::uintptr_t stride : {std::uintptr_t{144}, std::uintptr_t{160}}) {
        std::unordered_map<std::uintptr_t, std::size_t> same;   // fit35: counted once (was count x count)
        same.reserve(count * 2 + 16);
        const auto key_of = [&](const Hit& h) { return h.address - stride * rest_anchors[h.anchor].index; };
        for (std::size_t i = 0; i < count; ++i)
            if (!is_cap(r.hits[i]) && !is_charm(r.hits[i]) && !is_bump(r.hits[i])) ++same[key_of(r.hits[i])];
        for (std::size_t i = 0; i < count; ++i)
            if (!is_cap(r.hits[i]) && !is_charm(r.hits[i]) && !is_bump(r.hits[i]) && same[key_of(r.hits[i])] >= rest_anchors.size() / 2)
                r.hits[i].lasting = true;
    }
    // fit33 research: the first capsule hits with what is around them (which copies hold the radius, in which layout)
    static std::array<std::uintptr_t, 24> shown{};
    for (std::size_t i = 0; i < count && r.caps_shown < shown.size(); ++i) {
        if (!is_cap(r.hits[i]) || std::find(shown.begin(), shown.begin() + r.caps_shown, r.hits[i].address) != shown.begin() + r.caps_shown)
            continue;
        shown[r.caps_shown++] = r.hits[i].address;
        std::array<std::uint8_t, 96> around{};
        const bool got = memory::peek_bytes(r.hits[i].address - 48, around.data(), around.size());
        std::string hex;
        for (std::size_t b = 0; got && b < around.size(); ++b) hex += std::format("{}{:02x}", b && b % 16 == 0 ? " " : "", around[b]);
        note(std::format("coll: capsule {} at {:#x} (protect {:#x}): {}", r.hits[i].anchor & ~cap_flag, r.hits[i].address,
                         r.hits[i].protect, got ? hex : "unreadable"));
    }
    if (added) r.applied_at = 0; // written at once
    return added;
}
bool write_floats(std::uintptr_t address, const std::array<float, 3>& value) noexcept {
    __try {
        std::memcpy(reinterpret_cast<void*>(address), value.data(), 12);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool write_u32(std::uintptr_t address, std::uint32_t value) noexcept {
    __try {
        std::memcpy(reinterpret_cast<void*>(address), &value, 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// fit36: the charm picks packed for the atomic: byte s = option + 1 (0 none), bit 31 set; 0 = the defaults
using Charms = std::array<int, 3>;
std::uint32_t charm_pack(const Charms& p) noexcept {
    std::uint32_t v = 0x80000000u;
    for (std::size_t s = 0; s < 3; ++s) v |= static_cast<std::uint32_t>(std::clamp(p[s] + 1, 0, 255)) << (8 * s);
    return v;
}
// fit59: one set of bones per option, so an option hangs in one slot at most (a later slot holding it again: none)
Charms charm_unique(Charms p) noexcept {
    for (std::size_t s = 1; s < 3; ++s)
        for (std::size_t e = 0; e < s; ++e)
            if (p[s] >= 0 && p[s] == p[e]) p[s] = -1;
    return p;
}
Charms charm_unpack(std::uint32_t v) noexcept {
    if (!(v & 0x80000000u)) return charm_defaults;
    Charms p{};
    for (std::size_t s = 0; s < 3; ++s) {
        const int o = static_cast<int>((v >> (8 * s)) & 0xffu) - 1;
        p[s] = o >= 0 && o < static_cast<int>(charm_option_keys.size()) ? o : -1;
    }
    return charm_unique(p);
}
// saved as the options' keys ("key_brass,none,dice"): a later build may order or add options differently
std::string charm_text(const Charms& p) {
    std::string out;
    for (std::size_t s = 0; s < 3; ++s)
        out += std::string(s ? "," : "") + (p[s] >= 0 ? charm_option_keys[static_cast<std::size_t>(p[s])] : "none");
    return out;
}
Charms parse_charms(const std::string& text) {
    Charms p = charm_defaults;
    std::size_t s = 0, at = 0;
    while (s < 3 && at <= text.size()) {
        const auto end = std::min(text.find(',', at), text.size());
        const auto word = text.substr(at, end - at);
        if (word == "none") p[s] = -1;
        for (std::size_t o = 0; o < charm_option_keys.size(); ++o)
            if (word == charm_option_keys[o]) p[s] = static_cast<int>(o);
        ++s, at = end + 1;
    }
    return charm_unique(p);
}
// fit55: the braided model of an item (charm_models[]), -1 for none
int charm_model_of(const std::string& key) noexcept {
    const auto var = item_browser::accessory_variant(key);
    if (!var) return -1;
    for (std::size_t i = 0; i < charm_models.size(); ++i)
        if (var->model == charm_models[i].item) return static_cast<int>(i);
    return -1;
}
bool charm_slot_item(const std::string& key) {
    return key.starts_with("own_keychain_charm_slots_test") || charm_model_of(key) >= 0;
}
// fit51: the model of an item key (no place, no charm set, no quiet twin): colours / charms are kept per model
std::string look_key(std::string outfit) {
    if (const auto var = item_browser::accessory_variant(outfit)) outfit = var->model;
    if (const auto q = outfit.find("_quiet"); q != std::string::npos) outfit.erase(q, 6);
    return outfit;
}
std::string charm_key(std::string outfit) {   // the quiet twin and (fit48) every place keep the model's picks
    if (const auto var = item_browser::accessory_variant(outfit)) outfit = var->model;
    if (const auto q = outfit.find("_quiet"); q != std::string::npos) outfit.erase(q, 6);
    return "Accessories.Charms." + outfit;
}
std::array<float, 3> floats_from(const std::array<std::uint8_t, 12>& bytes) noexcept {
    std::array<float, 3> v{};
    std::memcpy(v.data(), bytes.data(), sizeof(v));
    return v;
}
Rows rows_from(const std::array<std::uint8_t, 36>& bytes) noexcept {
    Rows r{};
    std::memcpy(r.data(), bytes.data(), sizeof(r));
    return r;
}
Rows charm_rest_rows(const CharmPlace& t, std::size_t k) noexcept {   // pieces / contacts (options: not turned)
    if (k >= charm_piece0 && k < charm_ring0) return rows_from(t.pieces[k - charm_piece0].rows);
    if (k >= charm_ring0 && k < charm_cara0) return rows_from(t.rings[k - charm_ring0].rows);
    return Rows{};
}
// fit46 / fit48: the layout of the charms shown. A ring and its charm hang as ONE piece from the ring's contact on
// the carabiner's bar. What two neighbours' thickness leaves no room for (they hang at different depths on their
// rings) is held apart sideways: about half by the contacts sliding apart along the bar (3..10 mm), the rest by turning
// the pieces about their contacts (lever = ring + charm, so small angles). Ring places in 0.1 mm steps, whole degrees.
// fit59: an option's sizes as it hangs in that slot (its charm's place on its ring differs a little per slot)
const CharmFit* charm_opt(int slot, int option) noexcept {
    if (slot < 0 || slot > 2 || option < 0 || option >= static_cast<int>(charm_n)) return nullptr;
    const auto& o = cpt().opts[static_cast<std::size_t>(option)];
    return o.option == option ? &o.fit[static_cast<std::size_t>(slot)] : nullptr;
}
float charm_need(const CharmFit& a, const CharmFit& b) noexcept {   // how far apart sideways their centres must be
    const float t = (a.thick + b.thick) / 2.0f, gap = std::fabs(a.depth - b.depth);
    const float f = std::clamp((t - gap) / std::max(t, 0.001f), 0.0f, 1.0f);
    return (a.width + b.width) / 2.0f * std::max(f, 0.3f);
}
struct CharmLayout { std::array<float, 3> ring_dx{}; std::array<int, 3> angle{}; std::array<bool, 3> on{}; };
CharmLayout charm_layout(const Charms& c) noexcept {
    CharmLayout l{};
    std::array<const CharmFit*, 3> on{};
    std::array<int, 3> slots{};
    int n = 0;
    for (int sl = 0; sl < 3; ++sl)
        if (const auto* b = charm_opt(sl, c[static_cast<std::size_t>(sl)]))
            on[static_cast<std::size_t>(n)] = b, slots[static_cast<std::size_t>(n++)] = sl, l.on[static_cast<std::size_t>(sl)] = true;
    if (n < 2) return l;
    const auto q = [](float x) { return std::round(x * 10000.0f) / 10000.0f; };
    const auto lever = [](const CharmFit& b) { return charm_ring_h + b.drop; };
    const auto pair = [&](const CharmFit& a, const CharmFit& b, float share, float& slide) {
        const float need = charm_need(a, b);
        slide = std::clamp(0.5f * need, 0.003f, 0.010f);
        const float left = std::max(0.0f, need - slide) * share;
        const float s = left / std::max(0.02f, (lever(a) + lever(b)) / 2.0f);
        return static_cast<int>(std::lround(std::min(45.0f, std::asin(std::clamp(s, 0.0f, 0.8f)) * 180.0f / 3.14159265f)));
    };
    float slide{}, slide_r{};
    if (n == 2) {
        const int t = pair(*on[0], *on[1], 0.5f, slide);
        l.ring_dx[static_cast<std::size_t>(slots[0])] = q(-slide / 2.0f), l.ring_dx[static_cast<std::size_t>(slots[1])] = q(slide / 2.0f);
        l.angle[static_cast<std::size_t>(slots[0])] = -t, l.angle[static_cast<std::size_t>(slots[1])] = t;
    } else {   // the middle one hangs straight; each side piece takes all of what is left with it
        const int tl = pair(*on[0], *on[1], 1.0f, slide), tr = pair(*on[1], *on[2], 1.0f, slide_r);
        l.ring_dx[static_cast<std::size_t>(slots[0])] = q(-slide), l.ring_dx[static_cast<std::size_t>(slots[2])] = q(slide_r);
        l.angle[static_cast<std::size_t>(slots[0])] = -tl, l.angle[static_cast<std::size_t>(slots[2])] = tr;
    }
    return l;
}
// fit47 / fit48: how far a piece's rest is turned past its line for it to HANG on that line: every 1/30 s step gravity
// moves it g dt^2 / length x sin(angle) towards the vertical and the bend restore pulls it bend x (rest - angle) back;
// they balance at lead = g dt^2 sin(angle) / (length x bend), at most 0.9 of what its radius lets it leave the rest
// (beyond, the radius holds it there: towards the middle it stops at its line, outwards it swings free)
float charm_lead(int option, int degrees) noexcept {   // fit59: the option's own piece
    if (!degrees || option < 0 || option >= static_cast<int>(charm_n)) return 0.0f;
    const auto& p = cpt().pieces[static_cast<std::size_t>(option)];
    constexpr float pi = 3.14159265f, step = 1.0f / 30.0f;
    const float th = std::fabs(static_cast<float>(degrees)) * pi / 180.0f;
    const float pull = p.g * step * step / std::max(p.length, 0.004f) * std::sin(th) / std::max(charm_piece_bend, 0.005f);
    const float room = 0.9f * 2.0f * std::asin(std::clamp(charm_piece_radius / 2.0f, 0.0f, 1.0f));
    return std::copysign(std::round(std::min(pull, room) * 180.0f / pi), static_cast<float>(degrees));
}
// fit47: a bumper's radius for the two charms shown (what keeps their centres apart; slot_sim2: 0.85 x never
// touched, 1.0 x knocked the middle one ~30 % of the scenario); a slot empty: 2 mm
float bump_radius(const CharmBumper& bp, const Charms& c) noexcept {
    const auto* x = charm_opt(bp.from, c[bp.from]);
    const auto* y = charm_opt(bp.to, c[bp.to]);
    return x && y ? std::max(0.002f, charm_need(*x, *y)) : 0.002f;
}
// a piece's rest turned by `degrees` about its contact (translation and rows; row vectors: v' = v x q)
void piece_pose(const CharmPiece& p, float degrees, std::array<std::uint8_t, 12>& t, Rows& rows) noexcept {
    rows = rows_from(p.rows);
    t = p.base;
    if (degrees == 0.0f) return;
    const auto q = axis_rows(cpt().fan_axis, degrees * cpt().mirror);
    const auto v = floats_from(p.base);
    std::array<float, 3> r{};
    for (std::size_t j = 0; j < 3; ++j) r[j] = v[0] * q[j] + v[1] * q[3 + j] + v[2] * q[6 + j];
    std::memcpy(t.data(), r.data(), 12);
    rows = mul_rows(rows, q);
}
// a contact slid to bar place dx (m from the middle, along the bar's curve) and turned by the piece's angle
void ring_pose(const CharmRing& r, float dx, int degrees, std::array<std::uint8_t, 12>& t, Rows& rows) noexcept {
    const auto& tp = cpt();
    // fit55: on another model's carabiner its own bar: where its ring hangs against the camo's, and its curve
    const CharmModel* md = charm_model_cur >= 0 ? &charm_models[static_cast<std::size_t>(charm_model_cur)] : nullptr;
    const float curve = md ? md->bar_curve : charm_bar_curve;
    const float ddx = dx - r.bind_dx + (md ? md->ring_dx : 0.0f),
                ddy = curve * dx * dx - charm_bar_curve * r.bind_dx * r.bind_dx + (md ? md->ring_dy : 0.0f);
    const auto b = floats_from(r.base);
    std::array<float, 3> v{};
    for (std::size_t j = 0; j < 3; ++j) v[j] = b[j] + ddx * tp.ring_ex[j] + ddy * tp.ring_ey[j];
    std::memcpy(t.data(), v.data(), 12);
    rows = rows_from(r.rows);
    if (degrees != 0) rows = mul_rows(rows, axis_rows(tp.fan_axis, static_cast<float>(degrees) * tp.mirror));
}
// fit59: an option's charm as it hangs in that slot: its rest moved by the slot's shift (on its piece; middle slot: 0)
std::array<std::uint8_t, 12> charm_in_slot(const CharmOption& o, int slot) noexcept {
    auto v = floats_from(o.shown);
    for (std::size_t j = 0; j < 3; ++j) v[j] += o.shift[static_cast<std::size_t>(slot)][j];
    std::array<std::uint8_t, 12> t{};
    std::memcpy(t.data(), v.data(), 12);
    return t;
}
bool charm_known(std::size_t k, const std::array<std::uint8_t, 12>& t) noexcept {
    for (std::size_t i = 0; i < std::min(charm_seen_tn[k], charm_history); ++i)
        if (charm_seen_t[k][i] == t) return true;
    return false;
}
bool charm_known_rows(std::size_t k, const Rows& rows) noexcept {
    for (std::size_t i = 0; i < std::min(charm_seen_rn[k], charm_history); ++i)
        if (charm_seen_r[k][i] == rows) return true;
    return false;
}
void charm_remember(std::size_t k, const std::array<std::uint8_t, 12>& t, const Rows& rows) noexcept {
    if (!charm_known(k, t)) charm_seen_t[k][charm_seen_tn[k]++ % charm_history] = t;
    if (!charm_known_rows(k, rows)) charm_seen_r[k][charm_seen_rn[k]++ % charm_history] = rows;
}

// The fit of the pants worn, and what is worn (customizer state shared with the menu).
struct Worn {
    SRWLOCK lock = SRWLOCK_INIT;
    Fit fit{};                      // in force: the pants' own, or the one for all pants
    bool pants_own{};               // the pants have their own fit
    std::string outfit, pants;      // item keys (file part of the asset name)
    std::string shown;              // fit57: the keychain the skater shows (the grid's focused one, else the outfit)
    int shown_place = -1;
    std::uintptr_t entity{};        // fit49: the local skater entity the recipe was read from
    int place = -1;                 // the outfit's place when it is one of our keychains
    std::uint64_t changed_at{};     // the menu changed the fit then (saved and rescanned a moment later)
    bool unsaved{};
    std::string summary;            // the last scan / write line, for the menu
    std::size_t live{}, lasting{};
};
Worn& worn() { static auto* value = new Worn; return *value; }
std::string fit_text(const Fit& f) {
    std::string out;
    for (const auto& o : f)
        out += std::format("{}{:.2f},{:.2f},{:.2f},{:.1f},{:.1f},{:.1f}", out.empty() ? "" : ";", o.up, o.out, o.along, o.turn, o.tilt, o.coll);
    return out;
}
Offset clamped(Offset o) noexcept {
    // fit51 (user: "up to 10 cm every way, finer"): up +-15, off the leg / along +-10 cm (0.25 cm steps), tilt +-45
    return {std::clamp(o.up, -15.0f, 15.0f), std::clamp(o.out, -10.0f, 10.0f), std::clamp(o.along, -10.0f, 10.0f),
            std::clamp(o.turn, -60.0f, 60.0f), std::clamp(o.tilt, -45.0f, 45.0f), // fit24: the tilt again
            std::clamp(o.coll, -2.0f, 6.0f)};                                     // fit33
}
// "u,o,a[,turn,tilt[,coll]];..." per place; places or values missing are 0 (a v1 text has 8 places of 3).
std::optional<Fit> parse_fit(const std::string& text) {
    Fit f{};
    std::size_t place = 0, at = 0;
    while (at <= text.size() && place < f.size()) {
        const auto end = std::min(text.find(';', at), text.size());
        float v[6]{};
        std::size_t k = 0, p = at;
        while (p < end && k < 6) {
            char* stop{};
            const float x = std::strtof(text.c_str() + p, &stop);
            const auto used = static_cast<std::size_t>(stop - text.c_str());
            if (used == p || used > end || !std::isfinite(x)) return std::nullopt;
            v[k++] = x;
            p = used;
            if (p < end && text[p] == ',') ++p;
        }
        f[place++] = clamped({v[0], v[1], v[2], v[3], v[4], v[5]});
        at = end + 1;
    }
    return f;
}
std::string fit_key(const std::string& pants) { return pants.empty() ? std::string("Accessories.Fit") : "Accessories.Fit." + pants; }
std::optional<Fit> saved_fit(const std::string& pants) {
    const auto value = profile_runtime::local_value(fit_key(pants));
    if (!value || !value->is_string()) return std::nullopt;
    return parse_fit(value->get<std::string>());
}
std::string short_key(const std::string& asset) {
    const auto slash = asset.find_last_of('/');
    std::string out = slash == std::string::npos ? asset : asset.substr(slash + 1);
    for (auto& c : out) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return out;
}
int keychain_place(const std::string& key) {
    const auto ends = [&](std::string_view s) { return key.ends_with(s); };
    if (key.starts_with("own_lanyard_")) {   // (the lanyards' places follow the keychains')
        const int base = static_cast<int>(first_lanyard_place);
        if (ends("_front_right")) return base + 4;
        if (ends("_side_right")) return base + 5;
        if (ends("_right")) return base + 3;
        if (ends("_front")) return base + 1;
        if (ends("_side")) return base + 2;
        return base;
    }
    if (!key.starts_with("own_keychain_")) return -1;
    if (ends("_front_clip_right")) return 7;
    if (ends("_front_clip")) return 6;
    if (ends("_front_right")) return 4;
    if (ends("_side_right")) return 5;
    if (ends("_right")) return 3;
    if (ends("_front")) return 1;
    if (ends("_side")) return 2;
    return 0;
}
// fit57: the keychain the grid has focused (the wardrobe's skater shows it, not the worn one) - set by the overlay
// every frame while the focus is on one of ours
SRWLOCK shown_lock = SRWLOCK_INIT;
std::string shown_key;
std::atomic<std::uint64_t> shown_at{};
std::atomic<bool> shown_changed{};
// Client tick, twice a second (fit57: at once when the focused keychain changes): what the local skater wears (the
// cosmetic recipe), and the fit of those pants.
void tick_worn(std::uintptr_t base, std::uintptr_t client) {
    static std::uint64_t looked{};
    const bool focus_moved = shown_changed.exchange(false);
    if (!focus_moved && GetTickCount64() - looked < 500) return;
    looked = GetTickCount64();
    const auto local = multiplayer::capture_local(base, client, false);
    if (!local.ready) return;
    auto read = [](std::uintptr_t address, void* out, std::size_t size) { return memory::peek_bytes(address, out, size); };
    multiplayer::CosmeticRecipe recipe;
    try {
        multiplayer::CosmeticMemory<decltype(read)&> m{read, base};
        recipe = m.capture(local.entity);
    } catch (...) { return; }
    // The recipe names items by their keys ("Own_Keychain_Angular_Navy"), not by asset paths; its slots by hash
    // (worn log 2026-10-18: outfit 4170730925, bottoms 3228215319).
    constexpr std::uint32_t outfit_slot = 4170730925U, bottoms_slot = 3228215319U;
    std::string outfit, pants;
    for (const auto& item : recipe.items) {
        const auto key = short_key(item.asset);
        const bool keychain = key.starts_with("own_keychain_");
        if (item.slot == outfit_slot || keychain || key.starts_with("own_lanyard_") || key.starts_with("own_wallet_"))
            if (!key.empty() && (outfit.empty() || keychain || (item.slot == outfit_slot && !outfit.starts_with("own_keychain_"))))
                outfit = key;
        if (item.slot == bottoms_slot) pants = key;
    }
    std::string shown_item = outfit;   // fit57: while the grid has one of our keychains focused, the skater shows it
    if (GetTickCount64() < shown_at.load(std::memory_order_acquire) + 800) {
        AcquireSRWLockShared(&shown_lock);
        const auto shown = shown_key;
        ReleaseSRWLockShared(&shown_lock);
        if (shown.starts_with("own_keychain_")) shown_item = shown;
    }
    auto& w = worn();
    // The recipe as it changes (the own Accessories slot's tests: is the slot there, what does it hold).
    {
        static std::string last;
        std::string line;
        for (const auto& item : recipe.items) line += " [" + std::to_string(item.slot) + " " + item.asset + "]";
        if (line != last) last = line, note(std::format("worn ({} slots):{}", recipe.items.size(), line));
    }
    AcquireSRWLockExclusive(&w.lock);
    const bool pants_changed = pants != w.pants;
    const auto old_outfit = w.outfit;
    w.outfit = outfit, w.place = keychain_place(outfit);
    const auto old_shown = w.shown;
    w.shown = shown_item, w.shown_place = keychain_place(shown_item);
    w.entity = static_cast<std::uintptr_t>(local.entity);
    if (pants_changed) {
        // Unsaved edits of the pants before are kept: saved now, under their key.
        if (w.unsaved) profile_runtime::set_local_values({{fit_key(w.pants_own ? w.pants : std::string()), Json(fit_text(w.fit))}}), w.unsaved = false;
        w.pants = pants;
        const auto own = pants.empty() ? std::nullopt : saved_fit(pants);
        w.pants_own = own.has_value();
        w.fit = own ? *own : saved_fit({}).value_or(Fit{});
        w.changed_at = GetTickCount64();
    }
    ReleaseSRWLockExclusive(&w.lock);
    // fit51 (user: "with / without the strap, or at another place, the colour and the charms reset - keep what I
    // picked"): the same model at another place (or with its strap toggled) keeps the colour, the strap colour and the
    // charms as they are (and saves them for it); another model loads its own - saved per model now, an older save
    // per item still read.
    const bool same_model = shown_item != old_shown && !shown_item.empty() && !old_shown.empty() && look_key(shown_item) == look_key(old_shown);
    if (same_model) {
        auto& st = state();
        st.finish_unsaved.store(true), st.strap_unsaved.store(true);
        if (charm_slot_item(shown_item)) st.charm_unsaved.store(true);
    }
    if (shown_item != old_shown && !same_model) {
        const auto saved_number = [&](const char* what) -> std::optional<double> {
            for (const auto& key : {std::string(what) + look_key(shown_item), std::string(what) + shown_item})
                if (const auto saved = profile_runtime::local_value(key); saved && saved->is_number()) return saved->get<double>();
            return std::nullopt;
        };
        int finish = 0;
        if (const auto saved = shown_item.empty() ? std::nullopt : saved_number("Accessories.Finish."))
            finish = std::clamp(static_cast<int>(*saved), 0, static_cast<int>(finishes.size()) - 1);
        auto& st = state();
        st.finish.store(finish);
        if (finishes[static_cast<std::size_t>(finish)].chameleon) st.armed.store(true); // the angle needs the render hook
        int strap = 0;   // fit34
        if (const auto saved = shown_item.empty() ? std::nullopt : saved_number("Accessories.Strap."))
            strap = std::clamp(static_cast<int>(*saved), 0, static_cast<int>(strap_finishes.size()) - 1);
        st.strap.store(strap);
        if (strap_finishes[static_cast<std::size_t>(strap)].chameleon) st.armed.store(true);
        if (charm_slot_item(shown_item)) {   // fit36: the slots' picks of this keychain
            // fit55: until the player picks, each model shows its own charms
            const int mi = charm_model_of(shown_item);
            Charms chosen = mi >= 0 ? charm_models[static_cast<std::size_t>(mi)].defaults : charm_defaults;
            if (const auto saved = profile_runtime::local_value(charm_key(shown_item)); saved && saved->is_string())
                chosen = parse_charms(saved->get<std::string>());
            st.charms.store(charm_pack(chosen));
        }
    }
    if (pants_changed) note(std::format("fit: pants {} -> {}", pants.empty() ? "none" : pants, fit_text(worn().fit)));
    if (outfit != old_outfit) note(std::format("fit: outfit {} (place {})", outfit.empty() ? "none" : outfit, keychain_place(outfit)));
    if (shown_item != old_shown && shown_item != outfit) note(std::format("fit: shown {} (the grid's focus)", shown_item));
}
// ============================================================================ fit18: one tile per accessory model
// The grids show each model once (item_browser_order.h): its tile shows the row of the place picked in the panel
// ("Accessories.Place.<model>" = the place word), and the game equips that row's item when the player (or the panel,
// with a tap of A) picks the tile. fit18 changed the skater's recipe entry instead: the game put the outfit's item
// back each time (the skater takes its items from the outfit when told they changed), so nothing changed on screen.
struct Variants {
    SRWLOCK lock = SRWLOCK_INIT;
    std::map<std::string, std::string, std::less<>> picks;   // model -> place word (and charm_pick / sound_pick)
    std::set<std::string, std::less<>> unsaved;
    std::set<std::string, std::less<>> installed;            // fit28: lower case keys, for any thread
    // client thread
    std::uint64_t generation = ~std::uint64_t{};
    std::map<std::uint32_t, std::string> by_hash;            // our installed accessories: hash -> exact key
    std::map<std::string, std::uint32_t, std::less<>> by_key; // lower case key -> hash
    std::uint64_t looked{}, wrote_at{};
    std::uintptr_t wrote_component{};
    std::uint32_t wrote_hash{}, refused{};
};
Variants& variants() { static auto* value = new Variants; return *value; }
// fit20: Place2 - the fit18 / fit19 picks (some adopted from what the tests wore) are not carried over
std::string pick_key(std::string_view model) { return "Accessories.Place2." + std::string(model); }
void load_variants(Variants& v) {
    const auto& c = profile_runtime::cosmetic_runtime();
    if (v.generation == c.items_generation && !v.by_hash.empty()) return;
    v.generation = c.items_generation;
    v.by_hash.clear(), v.by_key.clear();
    std::set<std::string> models;
    for (const auto& [key, info] : c.items) {
        const auto var = item_browser::accessory_variant(key);
        if (!var || !info.hash) continue;
        v.by_hash[info.hash] = key;
        v.by_key[item_browser::folded(key)] = info.hash;
        models.insert(var->model);
    }
    std::map<std::string, std::string, std::less<>> saved;
    for (const auto& model : models) {
        if (const auto value = profile_runtime::local_value(pick_key(model)); value && value->is_string())
            if (const auto place = value->string(); item_browser::accessory_variant(model + place))
                saved[model] = place;
        // fit28: the model's charm set
        const auto charm = item_browser::charm_pick(model);
        if (const auto value = profile_runtime::local_value(pick_key(charm)); value && value->is_string())
            if (const auto set = value->string();
                std::find(std::begin(item_browser::charm_sets), std::end(item_browser::charm_sets), set) != std::end(item_browser::charm_sets))
                saved[charm] = set;
    }
    if (const auto value = profile_runtime::local_value(pick_key(item_browser::sound_pick)); value && value->is_string())
        saved[std::string(item_browser::sound_pick)] = value->string() == "off" ? "off" : "on";
    std::set<std::string, std::less<>> keys;
    for (const auto& [key, hash] : v.by_key) keys.insert(key);
    AcquireSRWLockExclusive(&v.lock);
    for (auto& [model, place] : saved) v.picks.try_emplace(model, place); // a pick made before the load wins
    v.installed = std::move(keys);
    ReleaseSRWLockExclusive(&v.lock);
    if (!v.by_hash.empty()) note(std::format("variants: {} accessory items of {} models, {} picks saved", v.by_hash.size(), models.size(), saved.size()));
}
// Client tick (at most every 100 ms): the picks saved.
void tick_variant(std::uintptr_t base, std::uintptr_t client) {
    auto& v = variants();
    const auto now = GetTickCount64();
    if (now - v.looked < 100) return;
    v.looked = now;
    load_variants(v);
    if (v.by_hash.empty()) return;
    {
        std::vector<std::pair<std::string, std::string>> save;
        AcquireSRWLockExclusive(&v.lock);
        for (const auto& model : v.unsaved)
            if (const auto found = v.picks.find(model); found != v.picks.end()) save.emplace_back(model, found->second);
        v.unsaved.clear();
        ReleaseSRWLockExclusive(&v.lock);
        for (const auto& [model, place] : save)
            try { profile_runtime::set_local_values({{pick_key(model), Json(place)}}); } catch (...) {}
    }
    const auto local = multiplayer::capture_local(base, client, false);
    if (!local.ready) return;
    auto read = [](std::uintptr_t address, void* out, std::size_t size) { return memory::peek_bytes(address, out, size); };
    multiplayer::CosmeticMemory<decltype(read)&> m{read, base};
    const auto component = m.component(local.entity);
    // Still working through a change (the game's or ours): looked at again next tick.
    if (m.get<std::uint8_t>(component, 0x10a) || m.get<std::uint8_t>(component, 0x10d)) return;
    const auto items = m.ptr(component, 0x158);
    const auto count = m.count(items, sizeof(multiplayer::NativeCosmeticItem), multiplayer::max_cosmetic_slots);
    for (std::size_t i = 0; i < count; ++i) {
        const auto at = items + i * sizeof(multiplayer::NativeCosmeticItem);
        const auto entry = m.get<multiplayer::NativeCosmeticItem>(at);
        const auto worn = v.by_hash.find(entry.hash);
        if (worn == v.by_hash.end()) continue;
        const auto var = item_browser::accessory_variant(worn->second);
        if (!var) continue;
        std::string place;
        bool picked{};
        AcquireSRWLockShared(&v.lock);
        if (const auto found = v.picks.find(var->model); found != v.picks.end()) place = found->second, picked = true;
        ReleaseSRWLockShared(&v.lock);
        (void)picked; // fit20: what is worn no longer becomes the pick (the tiles keep the defaults until picked)
        (void)place;
    }
}
// Client tick: the fit in force onto every live hit; scans when it is needed.
void tick_fit() {
    auto& r = rest();
    auto& w = worn();
    Fit fit;
    std::uint64_t changed_at{};
    bool save{};
    std::string pants;
    bool pants_own{};
    int worn_place{-1};
    AcquireSRWLockExclusive(&w.lock);
    fit = w.fit, changed_at = w.changed_at, pants = w.pants, pants_own = w.pants_own, worn_place = w.place;
    const auto now = GetTickCount64();
    if (w.unsaved && now - changed_at > 800) save = true, w.unsaved = false;
    ReleaseSRWLockExclusive(&w.lock);
    if (save) profile_runtime::set_local_values({{fit_key(pants_own ? pants : std::string()), Json(fit_text(fit))}});
    // A finish picked in a menu: kept for the keychain worn.
    if (auto& st = state(); st.finish_unsaved.exchange(false)) {
        AcquireSRWLockShared(&w.lock);
        const auto outfit = w.shown;   // fit57
        ReleaseSRWLockShared(&w.lock);
        if (!outfit.empty())
            profile_runtime::set_local_values({{"Accessories.Finish." + look_key(outfit), Json(static_cast<double>(st.finish.load()))}});   // fit51: per model
    }
    if (auto& st = state(); st.strap_unsaved.exchange(false)) {   // fit34
        AcquireSRWLockShared(&w.lock);
        const auto outfit = w.shown;   // fit57
        ReleaseSRWLockShared(&w.lock);
        if (!outfit.empty())
            profile_runtime::set_local_values({{"Accessories.Strap." + look_key(outfit), Json(static_cast<double>(st.strap.load()))}});   // fit51: per model
    }
    if (!panel_turn_sign_read.exchange(true)) {   // fit53: the stick's measured direction, from the profile
        try {
            if (const auto saved = profile_runtime::local_value("Accessories.PanelTurn"); saved && saved->is_number())
                panel_turn_sign_value.store(saved->get<double>() > 0 ? 1 : saved->get<double>() < 0 ? -1 : 0);
        } catch (...) {}
    }
    if (panel_turn_sign_unsaved.exchange(false)) {
        try { profile_runtime::set_local_values({{"Accessories.PanelTurn", Json(static_cast<double>(panel_turn_sign_value.load()))}}); } catch (...) {}
        note(std::format("panel: the right stick turns the skater {} (measured)", panel_turn_sign_value.load() > 0 ? "+" : "-"));
    }
    if (auto& st = state(); st.charm_unsaved.exchange(false)) {   // fit36
        AcquireSRWLockShared(&w.lock);
        const auto outfit = w.shown;   // fit57
        ReleaseSRWLockShared(&w.lock);
        if (charm_slot_item(outfit))
            profile_runtime::set_local_values({{charm_key(outfit), Json(charm_text(charm_unpack(st.charms.load())))}});
    }
    const Charms charms = charm_unpack(state().charms.load(std::memory_order_acquire));
    bool charm_turning = false;   // some charm bone or ring is to hold other than the asset
    static bool charm_rescan = false;
    {   // fit48: the place of the slot item worn picks the table; another place: its hits are no longer ours
        std::size_t cp = charm_place_cur;
        int cm = charm_model_cur;
        AcquireSRWLockShared(&w.lock);
        if (charm_slot_item(w.shown)) {   // fit57: what the skater shows
            for (std::size_t i = 0; i < charm_place_tables.size(); ++i)
                if (charm_place_tables[i].place == w.shown_place) cp = i;
            cm = charm_model_of(w.shown);   // fit55: and the model its carabiner / bar
        }
        ReleaseSRWLockShared(&w.lock);
        if (cm != charm_model_cur)
            note(std::format("charms: model {}", cm >= 0 ? charm_models[static_cast<std::size_t>(cm)].item : "test item"));
        if (cp != charm_place_cur || cm != charm_model_cur) {
            charm_place_cur = cp, charm_model_cur = cm, charm_want_set = false, charm_rescan = true;
            charm_seen_tn.fill(0), charm_seen_rn.fill(0);
            for (std::size_t i = 0; i < std::min(r.count.load(), most_hits); ++i)
                if (is_charm(r.hits[i]) || is_bump(r.hits[i])) r.hits[i].dead = true;
            note(std::format("charms: now the {} place's table", charm_place_tables[cp].place));
        }
    }
    {   // fit48: what every option, piece and contact of that place is to hold for these picks
        const auto& t = cpt();
        const auto lay = charm_layout(charms);
        std::array<int, charm_n> slot_of{};   // fit59: the slot each option is picked for (-1: none)
        slot_of.fill(-1);
        for (int sl = 0; sl < 3; ++sl)
            if (charms[static_cast<std::size_t>(sl)] >= 0) slot_of[static_cast<std::size_t>(charms[static_cast<std::size_t>(sl)])] = sl;
        for (std::size_t k = 0; k < charm_ids; ++k) {
            std::array<std::uint8_t, 12> want{}, built{};
            Rows rows{}, rest_r = charm_rest_rows(t, k);
            if (k < charm_piece0) {   // the charm: at its slot's place on its piece, or away
                const auto& o = t.opts[k];
                const int sl = slot_of[o.option];
                want = sl >= 0 ? charm_in_slot(o, sl) : o.hidden;
                built = o.hidden;   // the asset is built with no charm
            } else if (k < charm_ring0) {   // the piece: turned by its slot's lead (not picked: as built, its contact away)
                const auto& p = t.pieces[k - charm_piece0];
                const int sl = slot_of[p.option];
                if (sl >= 0) piece_pose(p, charm_lead(p.option, lay.angle[static_cast<std::size_t>(sl)]), want, rows);
                else want = p.base, rows = rest_r;
                built = p.base;
            } else if (k < charm_cara0) {   // the contact: at its slot's bar place, or 200 m away with its chain
                const auto& ring = t.rings[k - charm_ring0];
                const int sl = slot_of[ring.option];
                if (sl >= 0) ring_pose(ring, lay.ring_dx[static_cast<std::size_t>(sl)], lay.angle[static_cast<std::size_t>(sl)], want, rows);
                else want = ring.hidden, rows = rest_r;
                built = ring.hidden;
            } else {   // fit55: the worn model's carabiner shown, the others away (the asset shows the first: camo)
                const auto& c = t.caras[k - charm_cara0];
                const auto i = static_cast<int>(k - charm_cara0);
                want = i == std::max(charm_model_cur, 0) ? c.shown : c.hidden;
                built = want;   // fit58: the model's own asset shows its carabiner already
            }
            charm_turning = charm_turning || want != built || rows != rest_r;
            if (!charm_want_set) charm_was[k] = want, charm_want[k] = want, charm_was_rows[k] = rows, charm_want_rows[k] = rows;
            else if (want != charm_want[k] || rows != charm_want_rows[k])
                charm_was[k] = charm_want[k], charm_want[k] = want, charm_was_rows[k] = charm_want_rows[k], charm_want_rows[k] = rows;
            charm_remember(k, want, rows);
        }
        charm_want_set = true;
    }
    // fit33's thickness of our capsules: fit36 took its row out (no visible change: only the asset's capsule copy was
    // ever found), so the rest radii again; a thickness saved before is left in the profile, unused
    const float coll = 0.0f;
    const int phase = r.phase.load(std::memory_order_acquire);
    // When to scan: due times for quick scans, a full one when wanted.
    static std::array<std::uint64_t, 6> due{};
    static std::uint64_t full_due{}, seen_change{}, ready_at{}, watch_gap{1000};
    static std::uint64_t slot_watch_gap{3000};   // fit49
    if (charm_rescan) full_due = now + 200, charm_rescan = false;   // fit48: another place's slot item worn
    static std::string picked;
    static bool was_picking{};
    const auto plan = [&](std::uint64_t at) {
        for (auto& d : due)
            if (!d || d == at) { d = at; return; }
    };
    if (phase == 2 && r.reported != r.scans) {
        r.reported = r.scans;
        if (!(r.scan_applied == r.applied) || r.scan_coll != r.coll_applied) {
            plan(now); // the fit changed while it ran: what it found may hold the one before; again with this one
        } else {
            const auto added = take_scan(r);
            if (r.quick) watch_gap = added ? 1000 : std::min<std::uint64_t>(watch_gap * 2, 4000);
            if (r.quick && added) slot_watch_gap = 3000;   // fit49: copies still turning up - keep watching closely
            std::size_t lasting{}, caps{};
            for (std::size_t i = 0; i < std::min(r.count.load(), most_hits); ++i) lasting += r.hits[i].lasting, caps += is_cap(r.hits[i]);
            if (!r.quick)
                note(std::format("fit: scan {}: {} hits ({} lasting, {} capsules) in {} regions, {:.1f} GB, {} ms, {} faults", r.scans,
                                 r.count.load(), lasting, caps, r.regions, static_cast<double>(r.bytes.load()) / 1e9, r.ms, r.faults.load()));
            else if (added)
                note(std::format("fit: quick scan {}: {} new, {} hits ({} lasting), {} regions, {} MB, {} ms", r.scans, added,
                                 r.count.load(), lasting, r.regions, r.bytes.load() / 1000000, r.ms));
        }
    }
    const bool running = r.phase.load(std::memory_order_acquire) == 1;
    const bool used = fit_used(fit) || fit_used(r.applied) || coll != 0.0f || r.coll_applied != 0.0f ||
                      charms != charm_defaults || r.charms_applied != charm_defaults || charm_turning;
    std::string outfit;
    bool known{};
    {
        AcquireSRWLockShared(&w.lock);
        outfit = w.outfit, known = !w.pants.empty() || !w.outfit.empty();
        ReleaseSRWLockShared(&w.lock);
    }
    if (known && !ready_at) ready_at = now;
    // The game is ready (the skater's outfit read): the first full scan, so the long-lived copies are known.
    if (!r.full_at && !full_due && ready_at && now - ready_at > 3000) full_due = now;
    // A keychain of ours picked in a grid (the overlay says so every frame): quick scans now and while its preview
    // loads; a full one first when the last is old.
    auto& st = state();
    const bool picking = now < st.prepare_until.load(std::memory_order_acquire);
    const bool panel = now < st.focus_until.load(std::memory_order_acquire);
    if (picking && (!was_picking || outfit != picked)) {
        picked = outfit, watch_gap = 1000;
        if (!r.full_at || now - r.full_at > 30000) full_due = now;
        for (const std::uint64_t later : {0, 400, 1200, 2500}) plan(now + later);
    }
    was_picking = picking;
    {   // fit45: charm picks changed - FULL scans 0.3 s and 2.5 s later (a quick scan reads only the regions that held
        // hits; the panel character's new copies came up elsewhere holding an old pick: two charms in one slot)
        static Charms seen_charms = charm_defaults;
        static std::uint64_t charm_again{};
        if (charms != seen_charms) seen_charms = charms, full_due = now + 300, charm_again = now + 2500;
        if (charm_again && charm_again <= now && !full_due) full_due = now, charm_again = 0;
    }
    {   // fit49 (user: "the charms show in the menu, but in the game often not, then again"): entering the game makes a
        // new skater with new copies of our bones, and no scan ran until the menu was opened again (log 00:42:11 -
        // 00:42:58: in the game, no scans). A new skater entity: full scans 0.3 s and 2.5 s later; and while a slot
        // item is worn a slow watch (quick scans of the regions new since the last scan: every 3 s, doubling while
        // nothing new turns up, back to 3 s when something does). fit59 (user: more FPS): doubling up to 30 s, not 12 s
        static std::uintptr_t seen_entity{};
        static std::uint64_t entity_again{}, slot_watch_at{};
        std::uintptr_t ent{};
        bool slot{};
        AcquireSRWLockShared(&w.lock);
        ent = w.entity, slot = charm_slot_item(w.shown);
        ReleaseSRWLockShared(&w.lock);
        if (ent && ent != seen_entity) {
            if (seen_entity) {
                full_due = now + 300, entity_again = now + 2500, slot_watch_gap = 3000;
                note(std::format("fit: a new skater ({:#x} -> {:#x}): scans", seen_entity, ent));
            }
            seen_entity = ent;
        }
        if (entity_again && entity_again <= now && !full_due) full_due = now, entity_again = 0;
        if (slot && now - slot_watch_at > slot_watch_gap) {
            if (slot_watch_at) plan(now), slot_watch_gap = std::min<std::uint64_t>(slot_watch_gap * 2, 30000);
            slot_watch_at = now;
        }
    }
    // A change from the menu: the copies made since are found at once, and once more a moment later.
    if (changed_at > seen_change) {
        seen_change = changed_at, watch_gap = 1000;
        plan(changed_at + 150), plan(changed_at + 1500);
    }
    if (!running && !r.count.load()) r.applied = fit, r.coll_applied = coll, r.charms_applied = charms;
    if (!running) {
        bool quick_now = false;
        for (auto& d : due)
            if (d && d <= now) d = 0, quick_now = true;
        const bool watch = (picking || panel) && now - r.scan_at > watch_gap;
        std::size_t lasting{};
        for (std::size_t i = 0; i < std::min(r.count.load(), most_hits); ++i)
            if (!r.hits[i].dead) lasting += r.hits[i].lasting;
        if (full_due && full_due <= now) {
            full_due = 0;
            (void)start_scan(r.full_at ? "keychain picked" : "game ready");
        } else if (used && r.full_at && !lasting && now - r.full_at > 5000) {
            (void)start_scan("lasting copies gone");
        } else if (used && !r.full_at) {
            (void)start_scan("first use");
        } else if (quick_now || watch) {
            (void)start_scan("quick", true);
        }
    }
    if (!used) return; // nothing moved, nothing to do: no writes
    std::size_t live{}, lasting{};
    const bool change = fit != r.applied || coll != r.coll_applied || charms != r.charms_applied;
    if (!change && now - r.applied_at < 250) return;
    r.applied_at = now;
    std::size_t wrote{}, died{}, turned_n{}, unturnable{}, cap_wrote{}, cap_live{}, charm_wrote{}, charm_live{}, charm_odd{};
    std::size_t bump_wrote{}, bump_live{};
    live = 0, lasting = 0;
    for (std::size_t i = 0; i < std::min(r.count.load(), most_hits); ++i) {
        auto& hit = r.hits[i];
        if (hit.dead) continue;
        if (is_cap(hit)) {   // fit33: a capsule: its radius, while the record holds ours (rest or what we wrote)
            const auto& c = rest_capsules[hit.anchor & ~cap_flag];
            std::array<std::uint8_t, 12> held{};
            const auto rest_b = cap_bytes(c, 0.0f), was_b = cap_bytes(c, r.coll_applied);
            if (!memory::peek_bytes(hit.address, held.data(), 12) || (held != rest_b && held != was_b) ||
                ((hit.protect & 0xff) != PAGE_READWRITE && (hit.protect & 0xff) != PAGE_EXECUTE_READWRITE)) {
                hit.dead = true, ++died;
                continue;
            }
            const auto want = cap_radius(c, coll);
            std::uint32_t now_bits{};
            std::memcpy(&now_bits, held.data(), 4);
            if (now_bits != want) {
                if (!write_u32(hit.address, want)) { hit.dead = true, ++died; continue; }
                ++cap_wrote;
            }
            ++cap_live;
            continue;
        }
        if (is_bump(hit)) {   // fit47: a bumper input (its OffsetT matched): the radius (+16) for the charms shown
            const auto& bp = cpt().bumpers[static_cast<std::size_t>((hit.anchor & ~bump_flag) % cpt().bumpers.size())];
            std::array<std::uint32_t, 3> rec{};   // radius, usage, length
            if (!memory::peek_bytes(hit.address + 16, rec.data(), 12) || rec[2] != bp.length ||
                std::find(charm_bump_usages.begin(), charm_bump_usages.end(), rec[1]) == charm_bump_usages.end() ||
                ((hit.protect & 0xff) != PAGE_READWRITE && (hit.protect & 0xff) != PAGE_EXECUTE_READWRITE)) {
                hit.dead = true, ++died;
                continue;
            }
            const float want_r = bump_radius(bp, charms);
            std::uint32_t bits{};
            std::memcpy(&bits, &want_r, 4);
            if (rec[0] != bits) {
                if (!write_u32(hit.address + 16, bits)) { hit.dead = true, ++died; continue; }
                ++bump_wrote;
            }
            ++bump_live;
            continue;
        }
        if (is_charm(hit)) {   // fit46: a charm pendulum or a slot's ring: translation (and rows) as wanted now
            const auto k = static_cast<std::size_t>(hit.anchor & ~charm_flag);
            std::array<std::uint8_t, 12> held{};
            const bool read = memory::peek_bytes(hit.address, held.data(), 12);
            const bool plain = k < charm_ids && charm_plain(cpt(), k, held);
            if (k >= charm_ids || !read || !(plain || charm_known(k, held)) ||
                ((hit.protect & 0xff) != PAGE_READWRITE && (hit.protect & 0xff) != PAGE_EXECUTE_READWRITE)) {
                hit.dead = true, ++died;
                continue;
            }
            ++charm_live;
            if (held != charm_want[k]) {
                if (write_floats(hit.address, floats_from(charm_want[k]))) ++charm_wrote;
                else { hit.dead = true, ++died, ++charm_odd; continue; }
            }
            if (!hit.unturnable && k >= charm_piece0 && k < charm_cara0) {   // fit48: pieces and contacts turn; options / carabiners do not
                const auto now_rows = read_rows(hit.address);
                const Rows rest_r = charm_rest_rows(cpt(), k);
                if (!now_rows || (*now_rows != rest_r && !charm_known_rows(k, *now_rows)))
                    hit.unturnable = true, ++charm_odd;
                else if (*now_rows != charm_want_rows[k] && !write_rows(hit.address, charm_want_rows[k]))
                    hit.unturnable = true, ++charm_odd;
            }
            continue;
        }
        const auto& a = rest_anchors[hit.anchor];
        const auto rest_value = shifted(a, Offset{}), applied = shifted(a, r.applied[a.place]), want = shifted(a, fit[a.place]);
        std::array<float, 3> held{};
        if (!memory::peek_bytes(hit.address, held.data(), 12) || (held != rest_value && held != applied) ||
            ((hit.protect & 0xff) != PAGE_READWRITE && (hit.protect & 0xff) != PAGE_EXECUTE_READWRITE)) {
            hit.dead = true, ++died;
            continue;
        }
        if (held != want && !write_floats(hit.address, want)) { hit.dead = true, ++died; continue; }
        wrote += held != want;
        ++live, lasting += hit.lasting;
        // The turn: only where the rotation rows in front of the translation hold the anchor's rest rows or what was
        // written there (another copy's layout is left alone, for good).
        const auto& now_o = fit[a.place];
        const auto& was_o = r.applied[a.place];
        if (!hit.unturnable && (now_o.turn != 0.0f || now_o.tilt != 0.0f || was_o.turn != 0.0f || was_o.tilt != 0.0f)) {
            const auto rows_now = read_rows(hit.address);
            const auto rest_r = rest_rows(a), was_r = turned(a, was_o), want_r = turned(a, now_o);
            if (!rows_now || (*rows_now != rest_r && *rows_now != was_r)) hit.unturnable = true, ++unturnable;
            else if (*rows_now != want_r) {
                if (write_rows(hit.address, want_r)) ++turned_n;
                else hit.unturnable = true, ++unturnable;
            }
        }
    }
    const bool charms_changed = charms != r.charms_applied;
    r.applied = fit, r.coll_applied = coll, r.charms_applied = charms;
    if (charms_changed || charm_wrote || bump_wrote) {
        const auto lay = charm_layout(charms);
        std::array<float, 3> lead{};
        for (int sl = 0; sl < 3; ++sl)
            lead[static_cast<std::size_t>(sl)] = charm_lead(charms[static_cast<std::size_t>(sl)], lay.angle[static_cast<std::size_t>(sl)]);
        std::string bumps;
        for (const auto& bp : cpt().bumpers)
            if (bp.length) bumps += std::format("{}{}>{} {:.1f}", bumps.empty() ? "" : " / ", bp.from + 1, bp.to + 1, bump_radius(bp, charms) * 1000);
        note(std::format("charms {} at the {} place (contacts {:+.1f} / {:+.1f} / {:+.1f} mm, pieces {} / {} / {} deg, leads {:.0f} / "
                         "{:.0f} / {:.0f} deg): wrote {}, live {}, other layout {}; bumpers {} mm: wrote {}, live {}", charm_text(charms),
                         cpt().place, lay.ring_dx[0] * 1000, lay.ring_dx[1] * 1000, lay.ring_dx[2] * 1000, lay.angle[0], lay.angle[1],
                         lay.angle[2], lead[0], lead[1], lead[2], charm_wrote, charm_live, charm_odd, bumps, bump_wrote, bump_live));
    }
    if (change || died) {
        const auto line = std::format("fit: wrote {}, live {} ({} lasting), newly dead {}, turned {}, not turnable {}; "
                                      "capsules {:+.1f} cm: wrote {}, live {}; charms: live {}", wrote, live, lasting, died,
                                      turned_n, unturnable, coll, cap_wrote, cap_live, charm_live);
        AcquireSRWLockExclusive(&w.lock);
        w.summary = line, w.live = live, w.lasting = lasting;
        ReleaseSRWLockExclusive(&w.lock);
        if (change) note(line + " - " + fit_text(fit));
    }
}
} // namespace

void set_color(int choice) noexcept { state().color.store(std::clamp(choice, 0, static_cast<int>(picks.size()) - 1)); state().armed.store(true); }
void set_part(int part) noexcept { state().part.store(part == 0 || part == 1 ? part : 6); state().armed.store(true); }
void set_region(int region) noexcept { state().region.store(std::clamp(region, -1, 31)); state().armed.store(true); }
void set_lift(float centimetres) noexcept { state().lift.store(std::clamp(centimetres, -20.0f, 20.0f) / 100.0f); state().armed.store(true); }
void set_mode(int mode) noexcept { state().mode.store(mode == 2 ? 2 : 1); }
void request_dump() noexcept {
    auto& s = state();
    AcquireSRWLockExclusive(&s.lock);
    s.raw.filled = false;
    ReleaseSRWLockExclusive(&s.lock);
    s.want_raw.store(true);
    s.dump.store(true);
    s.armed.store(true);
}
void request_status() noexcept { state().status.store(true); state().armed.store(true); }
void request_params() noexcept { state().params.store(true); state().armed.store(true); }
void set_colormode(int mode) noexcept { state().colormode.store(mode); state().armed.store(true); }
void set_finish(int finish) noexcept {
    finish = std::clamp(finish, 0, static_cast<int>(finishes.size()) - 1);
    auto& s = state();
    s.finish.store(finish);
    s.finish_unsaved.store(true);
    if (finishes[static_cast<std::size_t>(finish)].chameleon) s.armed.store(true); // the angle needs the render hook
}
int current_finish() noexcept { return state().finish.load(); }
void set_strap(int strap) noexcept {
    strap = std::clamp(strap, 0, static_cast<int>(strap_finishes.size()) - 1);
    auto& s = state();
    s.strap.store(strap);
    s.strap_unsaved.store(true);
    if (strap_finishes[static_cast<std::size_t>(strap)].chameleon) s.armed.store(true);
}
int current_strap() noexcept { return state().strap.load(); }
bool charm_slots_available() noexcept {
    try {
        auto& w = worn();
        AcquireSRWLockShared(&w.lock);
        const bool on = charm_slot_item(w.shown);
        ReleaseSRWLockShared(&w.lock);
        return on;
    } catch (...) { return false; }
}
int charm_option_count() noexcept { return static_cast<int>(charm_option_keys.size()); }
const char* charm_option_name(int option) noexcept {
    return option >= 0 && option < static_cast<int>(charm_option_titles.size()) ? charm_option_titles[static_cast<std::size_t>(option)] : "None";
}
int current_charm(int slot) noexcept {
    if (slot < 0 || slot > 2) return -1;
    return charm_unpack(state().charms.load())[static_cast<std::size_t>(slot)];
}
void set_charm(int slot, int option) noexcept {
    if (slot < 0 || slot > 2) return;
    auto& s = state();
    auto chosen = charm_unpack(s.charms.load());
    const int o = option >= 0 && option < static_cast<int>(charm_option_keys.size()) ? option : -1;
    for (auto& other : chosen)   // fit59: an option hangs in one slot - picked here, it leaves the other
        if (o >= 0 && other == o) other = -1;
    chosen[static_cast<std::size_t>(slot)] = o;
    s.charms.store(charm_pack(chosen));
    s.charm_unsaved.store(true);
    auto& w = worn();   // the copies made since are found at once (tick_fit's quick scans after a change)
    AcquireSRWLockExclusive(&w.lock);
    w.changed_at = GetTickCount64();
    ReleaseSRWLockExclusive(&w.lock);
}
void step_charm(int slot, int step) noexcept {   // fit59: None, then the options not hung in another slot
    if (slot < 0 || slot > 2 || !step) return;
    const auto chosen = charm_unpack(state().charms.load());
    const int n = static_cast<int>(charm_option_keys.size()) + 1;
    int o = chosen[static_cast<std::size_t>(slot)];
    for (int tries = 0; tries < n; ++tries) {
        o = (o + 1 + (step > 0 ? 1 : -1) + n) % n - 1;
        bool elsewhere = false;
        for (int s = 0; s < 3; ++s)
            elsewhere = elsewhere || (s != slot && o >= 0 && chosen[static_cast<std::size_t>(s)] == o);
        if (!elsewhere) break;
    }
    set_charm(slot, o);
}
bool strap_available() noexcept { return state().has_strap.load(std::memory_order_acquire); }
const char* finish_name(int finish) noexcept {
    return finish >= 0 && finish < static_cast<int>(finishes.size()) ? finishes[static_cast<std::size_t>(finish)].name : "";
}
int finish_count() noexcept { return static_cast<int>(finishes.size()); }
int strap_count() noexcept { return static_cast<int>(strap_finishes.size()); }   // fit58
const char* strap_name(int strap) noexcept {
    return strap >= 0 && strap < static_cast<int>(strap_finishes.size()) ? strap_finishes[static_cast<std::size_t>(strap)].name : "Own";
}
std::array<float, 3> strap_swatch(int strap) noexcept {
    if (strap <= 0 || strap >= static_cast<int>(strap_finishes.size())) return {0.42f, 0.40f, 0.38f};   // own: the braid's
    const auto& c = strap_finishes[static_cast<std::size_t>(strap)].stops[0];
    return {std::sqrt(c[0]), std::sqrt(c[1]), std::sqrt(c[2])};
}
std::array<float, 3> finish_swatch(int finish) noexcept {
    if (finish <= 0 || finish >= static_cast<int>(finishes.size())) return {0.78f, 0.79f, 0.82f};   // own: chrome
    const auto& c = finishes[static_cast<std::size_t>(finish)].stops[0];
    return {std::sqrt(c[0]), std::sqrt(c[1]), std::sqrt(c[2])};                                     // linear -> roughly sRGB
}
void set_brightness(float value) noexcept { state().brightness.store(std::clamp(value, 0.3f, 1.6f)); }
void request_colortest() noexcept { state().test_wanted.store(true); state().armed.store(true); }
void request_scan() noexcept { state().scan_wanted.store(true); }
int panel_turn_sign() noexcept { return panel_turn_sign_value.load(std::memory_order_acquire); }
void set_panel_turn_sign(int sign) noexcept {
    panel_turn_sign_value.store(sign > 0 ? 1 : sign < 0 ? -1 : 0, std::memory_order_release);
    panel_turn_sign_unsaved.store(true, std::memory_order_release);
}
void panel_note(const std::string& line) noexcept {   // fit54
    try { note(line); } catch (...) {}
}
bool panel_turn_error(float& radians) noexcept {   // fit52
    const auto& s = state();
    radians = s.turn_error.load(std::memory_order_acquire);
    return GetTickCount64() - s.turn_error_at.load(std::memory_order_acquire) < 300;
}
void prepare_fit() noexcept { state().prepare_until.store(GetTickCount64() + 1500, std::memory_order_release); }
void set_shown_item(std::string_view key) noexcept {
    try {
        std::string k(key);
        for (auto& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        AcquireSRWLockExclusive(&shown_lock);
        if (k != shown_key) shown_key = k, shown_changed.store(true, std::memory_order_release);
        ReleaseSRWLockExclusive(&shown_lock);
        shown_at.store(GetTickCount64(), std::memory_order_release);
    } catch (...) {}
}
void set_focus(int place, float aspect) noexcept {
    auto& s = state();
    if (std::isfinite(aspect) && aspect > 0.5f) s.focus_aspect.store(aspect, std::memory_order_release);
    s.focus_place.store(place, std::memory_order_release);
    s.focus_until.store(place >= 0 ? GetTickCount64() + 300 : 0, std::memory_order_release);
}

FitView fit_view() noexcept {
    FitView v;
    auto& w = worn();
    try {
        AcquireSRWLockShared(&w.lock);
        v.place = w.place, v.outfit = w.outfit, v.pants = w.pants, v.pants_own = w.pants_own;
        if (w.place >= 0) v.offset = w.fit[static_cast<std::size_t>(w.place)];
        v.live = w.live, v.lasting = w.lasting, v.summary = w.summary;
        ReleaseSRWLockShared(&w.lock);
    } catch (...) { ReleaseSRWLockShared(&w.lock); }
    v.scanning = rest().phase.load() == 1 && !rest().quick && !rest().count.load();
    return v;
}
Offset place_offset(int place) noexcept {
    if (place < 0 || place >= static_cast<int>(place_count)) return {};
    auto& w = worn();
    AcquireSRWLockShared(&w.lock);
    const auto o = w.fit[static_cast<std::size_t>(place)];
    ReleaseSRWLockShared(&w.lock);
    return o;
}
bool pants_own() noexcept {
    auto& w = worn();
    AcquireSRWLockShared(&w.lock);
    const bool own = w.pants_own && !w.pants.empty();
    ReleaseSRWLockShared(&w.lock);
    return own;
}
bool pants_worn() noexcept {
    auto& w = worn();
    AcquireSRWLockShared(&w.lock);
    const bool on = !w.pants.empty();
    ReleaseSRWLockShared(&w.lock);
    return on;
}
int place_of(std::string_view key) noexcept {
    try {
        std::string k(key);
        for (auto& c : k) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        return keychain_place(k);
    } catch (...) { return -1; }
}
const char* place_name(int place) noexcept {
    return place >= 0 && place < static_cast<int>(place_count) ? place_names[static_cast<std::size_t>(place)] : "";
}
ModelPick model_pick(std::string_view key) noexcept {
    try {
        const auto var = item_browser::accessory_variant(key);
        if (!var) return {};
        ModelPick out{var->model, static_cast<int>(var->kind), std::string(item_browser::default_place(var->kind))};
        auto& v = variants();
        std::set<std::string> sets, places;
        AcquireSRWLockShared(&v.lock);
        if (const auto found = v.picks.find(var->model); found != v.picks.end()) out.place = found->second;
        if (const auto found = v.picks.find(item_browser::charm_pick(var->model)); found != v.picks.end()) out.charm = found->second;
        if (const auto found = v.picks.find(item_browser::sound_pick); found != v.picks.end()) out.quiet = found->second == "off";
        // fit28: what the model comes as (the installed keys of the model: a few hundred, under the lock)
        for (auto at = v.installed.lower_bound(var->model); at != v.installed.end() && at->starts_with(var->model); ++at)
            if (const auto other = item_browser::accessory_variant(*at); other && other->model == var->model) {
                sets.insert(other->charm);
                places.insert(other->place);
                out.has_quiet = out.has_quiet || other->quiet;
            }
        ReleaseSRWLockShared(&v.lock);
        for (const auto set : item_browser::charm_sets)
            if (sets.contains(std::string(set))) out.charms.emplace_back(set);
        // fit47 (user: on the charm slots test Place / Strap / Up changed nothing): that item is only at the back - the
        // place row picked places it has no item for and the fit went there. A pick for such a place falls back to
        // the default place, else the first installed one.
        for (const auto p : item_browser::accessory_places(var->kind))
            if (places.contains(std::string(p))) out.places.emplace_back(p);
        if (!out.places.empty() && std::find(out.places.begin(), out.places.end(), out.place) == out.places.end()) {
            const std::string fallback(item_browser::default_place(var->kind));
            out.place = std::find(out.places.begin(), out.places.end(), fallback) != out.places.end() ? fallback : out.places.front();
        }
        return out;
    } catch (...) { return {}; }
}
void pick_charm(std::string_view model, std::string_view charm) noexcept {
    try {
        auto& v = variants();
        AcquireSRWLockExclusive(&v.lock);
        v.picks[item_browser::charm_pick(model)] = std::string(charm);
        v.unsaved.insert(item_browser::charm_pick(model));
        ReleaseSRWLockExclusive(&v.lock);
    } catch (...) {}
}
void pick_sound(bool on) noexcept {
    try {
        auto& v = variants();
        AcquireSRWLockExclusive(&v.lock);
        v.picks[std::string(item_browser::sound_pick)] = on ? "on" : "off";
        v.unsaved.insert(std::string(item_browser::sound_pick));
        ReleaseSRWLockExclusive(&v.lock);
    } catch (...) {}
}
bool installed(std::string_view key) noexcept {
    try {
        auto& v = variants();
        AcquireSRWLockShared(&v.lock);
        const bool found = v.installed.contains(item_browser::folded(key));
        ReleaseSRWLockShared(&v.lock);
        return found;
    } catch (...) { return false; }
}
std::map<std::string, std::string, std::less<>> model_picks() noexcept {
    try {
        auto& v = variants();
        AcquireSRWLockShared(&v.lock);
        auto out = v.picks;
        ReleaseSRWLockShared(&v.lock);
        return out;
    } catch (...) { return {}; }
}
void pick_place(std::string_view model, std::string_view place) noexcept {
    try {
        if (!item_browser::accessory_variant(std::string(model) + std::string(place))) return;
        auto& v = variants();
        AcquireSRWLockExclusive(&v.lock);
        v.picks[std::string(model)] = std::string(place);
        v.unsaved.insert(std::string(model));
        ReleaseSRWLockExclusive(&v.lock);
    } catch (...) {}
}
int fit_place(int kind, std::string_view place) noexcept {
    if (kind < 0 || kind == static_cast<int>(item_browser::AccessoryKind::wallet)) return -1;
    const auto places = item_browser::accessory_places(static_cast<item_browser::AccessoryKind>(kind));
    for (std::size_t i = 0; i < places.size(); ++i)
        if (places[i] == place) return static_cast<int>(i + (kind == static_cast<int>(item_browser::AccessoryKind::lanyard) ? first_lanyard_place : 0));
    return -1;
}
bool place_is_lanyard(int place) noexcept {
    return place >= static_cast<int>(first_lanyard_place) && place < static_cast<int>(place_count);
}
void set_offset(int place, Offset offset) noexcept {
    if (place < 0 || place >= static_cast<int>(place_count)) return;
    offset = clamped(offset);
    auto& w = worn();
    AcquireSRWLockExclusive(&w.lock);
    if (!(w.fit[static_cast<std::size_t>(place)] == offset)) {
        w.fit[static_cast<std::size_t>(place)] = offset;
        w.changed_at = GetTickCount64(), w.unsaved = true;
    }
    ReleaseSRWLockExclusive(&w.lock);
}
void set_pants_own(bool own) noexcept {
    auto& w = worn();
    std::string key;
    std::string text;
    AcquireSRWLockExclusive(&w.lock);
    if (!w.pants.empty() && own != w.pants_own) {
        w.pants_own = own;
        try {
            if (own) {
                key = fit_key(w.pants), text = fit_text(w.fit);          // these pants keep what is shown now
            } else {
                key = fit_key(w.pants);                                  // forgotten: the fit for all pants again
                w.fit = saved_fit({}).value_or(Fit{});
                w.changed_at = GetTickCount64();
            }
        } catch (...) {}
        w.unsaved = false;
    }
    ReleaseSRWLockExclusive(&w.lock);
    if (!key.empty()) try { profile_runtime::set_local_values({{key, Json(text)}}); } catch (...) {}
}

// fit60 (2026-10-22, user: someone from a Discord group must not play with our mod): the Discord account this PC's
// app is signed into (discord_presence, from Discord's own READY - nothing is asked, nothing is sent anywhere) against
// SHA-256 hashes of lock_salt + "u:<username>" / "i:<user ID>". A match is kept in the profile (the lock holds when
// Discord is closed later) and, while the Physical_Lanyards mod is applied, our accessory features stay off and the
// overlay covers the screen with a notice (access_lock_overlay.cpp) until the player turns the mod off.
constexpr std::string_view lock_salt = "forestmouse/physical_lanyards/lock/1:";
// fit60d / fit60e: each listed account (username / ID hash) with the notice's title and second line for it
struct LockEntry { std::string_view hash; const char* title; const char* line; };
constexpr const char* lock_jerk = "You are not allowed to use this mod, u jerk";
constexpr const char* lock_plain = "Physical Lanyards";
constexpr std::array<LockEntry, 6> lock_hashes{{
    {"348c40d74df6df8dfee69875ee12690f4cc08a20af67e0fcf0363b07e832c9b8", lock_jerk, "Should've watched your mouth."},
    {"51b4f58ae03dd36a3e36099a38d1e7afeab59fa1ac782948233037a762fb438a", lock_jerk, "Should've watched your mouth."},
    {"26654bc54733252b39e2138b58c9e933f3a059ff47c1a9aa19df458de7f2bef7", lock_jerk, "Furries not allowed."},
    {"cc1f2ce822c070d43d02c618eabca4f1d9a5e4e2ec105833da48de091078bdb6", lock_jerk, "Furries not allowed."},
    {"a5623380d0f7cd0c52e13789c8962aeb74a3fa39c32d76dff9ccb5a84ebf05c5", lock_plain, "This mod is not available for your account."},
    {"3ed9e51d5c06905b8b78fa044e52dcab1d1e9c896cd1dc48b35d71d20190b9e9", lock_plain, "This mod is not available for your account."},
}};
std::atomic<bool> access_lock_on{};
std::atomic<const LockEntry*> access_lock_entry{};
// fit60b (user: "and a pig squeal like Kaspersky's"): our own generated squeal (research\access_lock), when the notice
// comes up and every 10 s while it stays
#include "access_lock_sound.inc"
void lock_squeal() noexcept {
    PlaySoundW(reinterpret_cast<LPCWSTR>(access_lock_sound), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}
std::string lock_hash(std::string_view text) {
    const auto salted = std::string(lock_salt) + std::string(text);
    std::array<unsigned char, 32> digest{};
    if (!BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                                   reinterpret_cast<PUCHAR>(const_cast<char*>(salted.data())), static_cast<ULONG>(salted.size()),
                                   digest.data(), static_cast<ULONG>(digest.size()))))
        return {};
    std::string hex;
    for (const auto b : digest) hex += std::format("{:02x}", b);
    return hex;
}
const LockEntry* lock_listed(std::string_view hash) noexcept {
    for (const auto& e : lock_hashes)
        if (!hash.empty() && e.hash == hash) return &e;
    return nullptr;
}
bool lanyards_applied() {
    for (auto name : live_mods::applied_mods()) {
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (name.find("physical_lanyards") != std::string::npos) return true;
    }
    return false;
}
void check_access() {
    static std::uint64_t at{};
    static std::string matched;   // the listed hash this PC matched (Discord now, or kept in the profile)
    const auto now = GetTickCount64();
    if (at && now - at < 2000) return;
    at = now;
    if (matched.empty()) {
        const auto who = discord_presence::account();
        auto name = who.username;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (const auto& h : {name.empty() ? std::string() : lock_hash("u:" + name), who.id.empty() ? std::string() : lock_hash("i:" + who.id)})
            if (lock_listed(h)) matched = h;
        if (!matched.empty()) {
            try { profile_runtime::set_local_values({{"Accessories.Lock", Json(matched)}}); } catch (...) {}
        } else if (const auto kept = profile_runtime::local_value("Accessories.Lock"); kept && kept->is_string() &&
                   lock_listed(kept->get<std::string>())) {
            matched = kept->get<std::string>();
        }
        if (const auto* e = lock_listed(matched)) access_lock_entry.store(e, std::memory_order_release);
    }
    const bool on = !matched.empty() && lanyards_applied();
    if (on != access_lock_on.exchange(on, std::memory_order_acq_rel))
        note(on ? "Physical Lanyards is not available for this account: its features are off, the notice is shown"
                : "Physical Lanyards is off: the notice is gone");
    static std::uint64_t squeal_at{};
    if (!on) squeal_at = 0;
    else if (!squeal_at || now - squeal_at >= 10000) squeal_at = now, lock_squeal();   // user: every 10 s
}

bool access_locked() noexcept { return access_lock_on.load(std::memory_order_acquire); }
const char* access_lock_title() noexcept {
    const auto* e = access_lock_entry.load(std::memory_order_acquire);
    return e ? e->title : lock_plain;
}
const char* access_lock_line() noexcept {
    const auto* e = access_lock_entry.load(std::memory_order_acquire);
    return e ? e->line : "";
}

void tick(std::uintptr_t base, std::uintptr_t client, bool ready) noexcept {
    auto& s = state();
    if (!base) return;
    try { check_access(); } catch (...) {}
    if (access_locked()) return;
    try {
        // The render hooks: for the research probes (armed) and the keychain panel's camera.
        const bool focus = GetTickCount64() < s.focus_until.load(std::memory_order_acquire) + 2000;
        if ((focus || s.armed.load(std::memory_order_acquire)) && !s.prepared) {
            for (auto& copy : s.ring) copy.resize(most_bones);
            for (auto& shot : s.snapshots) shot.bones.reserve(most_bones);
            s.base.store(base, std::memory_order_release);
            hall_of_meat::set_packet_editor(&edit_packet);
            hall_of_meat::set_view_editor(&edit_view);
            s.prepared = true;
            note("render editors set (the panel's camera, the probes)");
        }
        if (s.prepared && !s.hooked && (focus || s.armed.load(std::memory_order_acquire))) {
            s.hooked = hall_of_meat::hook_render_for_accessories(true);
            note(s.hooked ? "renderer hook on" : "renderer hook unavailable (Hall of Meat could not prepare it)");
        }
        if (focus && ready) {
            const auto local = multiplayer::capture_local(base, client, false);
            if (local.ready) {
                for (std::size_t mode = 0; mode < render::render_modes; ++mode)
                    s.indices[mode].store(hall_of_meat::skater_render_index(local.entity, mode), std::memory_order_release);
                s.indices_until.store(GetTickCount64() + lease_ms, std::memory_order_release);
            }
        }
        if (s.fov_seen_count >= 6 && !s.fov_logged) {
            s.fov_logged = true;
            note(std::format("panel camera: the view's field of view over the first frames {:.4f} {:.4f} {:.4f} {:.4f} {:.4f} {:.4f}",
                             s.fov_seen[0], s.fov_seen[1], s.fov_seen[2], s.fov_seen[3], s.fov_seen[4], s.fov_seen[5]));
        }
        {
            // What the panel camera did lately, every 2 s while it works.
            static std::uint64_t logged{};
            auto& c = focus_camera();
            if (GetTickCount64() - logged > 2000) {
                logged = GetTickCount64();
                AcquireSRWLockExclusive(&c.lock);
                const auto calls = c.calls, game_new = c.game_new, ours_seen = c.ours_seen, kept = c.kept, switches = c.switches, views = c.views;
                const float jump = c.jump_mm, dt_min = c.dt_min, dt_max = c.dt_max, blend = c.blend;
                c.calls = c.game_new = c.ours_seen = c.kept = c.switches = 0;
                c.jump_mm = 0.0f, c.dt_min = 1.0f, c.dt_max = 0.0f;
                ReleaseSRWLockExclusive(&c.lock);
                if (calls && blend > 0.0f)
                    note(std::format("panel camera, 2 s: {} views made, game camera new {} / ours still {}, kept last {}, skeleton "
                                     "switches {}, view buffers {}, aim jump max {:.1f} mm, frame {:.1f}..{:.1f} ms",
                                     calls, game_new, ours_seen, kept, switches, views, jump, dt_min * 1000.0f, dt_max * 1000.0f));
            }
        }
        if (ready) tick_worn(base, client);
        if (ready) try { tick_variant(base, client); } catch (...) {}
        if (s.scan_wanted.exchange(false)) (void)start_scan("asked");
        tick_fit();
        if (ready && !s.test_running && !s.armed.load(std::memory_order_acquire)) tick_finish(base, client);
    } catch (...) {}
    if (!s.armed.load(std::memory_order_acquire)) return;
    try {
        multiplayer::NativeFrame local;
        if (ready) local = multiplayer::capture_local(base, client, false);
        if (local.ready) {
            for (std::size_t mode = 0; mode < render::render_modes; ++mode)
                s.indices[mode].store(hall_of_meat::skater_render_index(local.entity, mode), std::memory_order_release);
            s.indices_until.store(GetTickCount64() + lease_ms, std::memory_order_release);
        }
        static std::uint64_t searched{};
        const bool report = s.status.exchange(false);
        if (report || GetTickCount64() - searched > 1000) {
            searched = GetTickCount64();
            search(report);
        }
        if (report)
            note(std::format("status: lift {:.1f} cm mode {} colour {} ({}) region {}; packets {} (render objects {}) skater {} "
                             "redirected {} in place {} skipped {}; skater render indices {} {}",
                             s.lift.load() * 100.0f, s.mode.load(), s.color.load(), pick_names[s.color.load()], s.region.load(),
                             s.packets.load(), s.ours.load(), s.skater_packets.load(), s.redirected.load(), s.in_place.load(),
                             s.skipped.load(), static_cast<int>(s.indices[0].load()), static_cast<int>(s.indices[1].load())));
        if (s.dump.load()) {
            bool filled{};
            AcquireSRWLockShared(&s.lock);
            filled = s.raw.filled;
            ReleaseSRWLockShared(&s.lock);
            static std::uint64_t asked{};
            if (!asked) asked = GetTickCount64();
            if (filled || GetTickCount64() - asked > 2000) {
                s.dump.store(false), s.want_raw.store(false), asked = 0;
                dump();
            }
        }
        if (s.test_wanted.exchange(false) && !s.test_running) {
            s.test_running = true, s.test_began = GetTickCount64(), s.test_shown = -1, s.test_saved.clear(), s.test_modes.clear();
            s.finish.store(0), s.color.store(0); // the test starts from the item's own look
            note("colour test: started (3 s a step)");
        }
        if (ready && s.test_running) tick_colortest(base, client);
        if (ready && !s.test_running) tick_colors(base, client);
        if (ready) tick_params(base, client);
        if (ready && !s.test_running) tick_finish(base, client);
    } catch (...) {}
}
} // namespace dingosdk::accessory_probe
