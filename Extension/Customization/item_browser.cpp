#include "item_browser.h"
#include "item_origins.h"
#include "accessory_probe.h"
#include "Engine/Core/Platform/path_text.h"
#include "local_customization_runtime.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Abi/native_data.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Vfs/content_catalogs.h"
#include "Extension/Multiplayer/Hud/game_ui_state.h"
#include "Extension/UI/NativeMenu/native_menu_data.h"
#include "Extension/UI/NativeMenu/native_menu_lifetime.h"
#include "Extension/UI/Overlay/overlay.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <thread>
#include <utility>

namespace dingosdk::item_browser {
namespace {
namespace md = dingosdk::multiplayer::menu_data;
using logging::Channel;
using logging::Level;

// Schema and field hashes are the game's own: a data-defined type's TypeNameHash and its fields'
// NameHash (UI/Foundations/Components/Lists/GridList/Widget/GridListViewModel and the types it holds).
constexpr md::Schema grid_list{0x7422aa5b, 440};
constexpr std::uint32_t owned_items = 0x345b75b1;   // GridListViewModel.OwnedItems
constexpr std::uint32_t tile_row = 0xcbe47f59;      // ContentPresenterTileViewModel, one per tile, inline
constexpr std::uint32_t ownable_view = 0x6e591955;  // UI/Features/Ownables/Widgets/OwnableViewModel
constexpr std::uint32_t ownable_data = 0x389117ef;  // the catalog item a tile shows
constexpr std::uint32_t reference = 0x62088281;     // DataModelReference
// ContentPresenterTileViewModel.ContentPresenter -> the model its widget shows.
constexpr std::array<std::uint32_t, 2> content_path{0x214d4984, 0x25e4d6c8};
// ContentPresenterTileViewModel.Button -> ... -> IsFocused, which the tile's widget keeps up to date.
constexpr std::array<std::uint32_t, 5> focused_path{0x0fb0d794, 0xa704272a, 0xc52416ef, 0xa704272a, 0x85a0b4e8};
constexpr std::uint32_t ownable_data_ref = 0xbbe34156; // OwnableViewModel.UIOwnableDataRef
constexpr std::uint32_t data_key = 0x7199d8ed, data_title = 0xec725743, data_category = 0x3389e313;
// OwnableViewModel.OwnableStyleSet.RarityStyle -> the style whose Icon the tile shows in its corner.
constexpr std::array<std::uint32_t, 2> rarity_path{0x35994418, 0x85839b5a};
constexpr md::Schema rarity_style{0xb6800075, 416};
constexpr std::uint32_t rarity_icon = 0xd4f0f044;
// The item card shows an item's description as a notice, and the game's focus script
// (ui/features/mystuff/cas/actions/casgriditem_onfocus_inventorymanager) gives it a warning sign
// (IconOverride: ui/textures/icons/generic/img_generic_warning_128) each time an item is
// focused. A mod item's description is only room for the overlay's mod card
// (item_origins::reserved_description), so that notice shows the game's info sign instead. Never
// none: the game resolves the override again later, and an empty one crashed it there
// (Skate.exe+0x3e897fb, a lookup that finds nothing; 2.1.0, 2026-10-18).
constexpr std::uint32_t notice_view = 0x7290e514;  // ui/foundations/components/notices/NoticeViewModel
constexpr std::uint32_t notice_message = 0x94703efc, label_text = 0x4d8e01b9; // .Message (a label) .Text
constexpr std::uint32_t notice_icon = 0x3514a765;  // NoticeViewModel.IconOverride
constexpr char notice_info_texture[] = "ui/textures/icons/generic/img_generic_info_64";
// The game's own star (UI/Textures/Common/Shapes/Bespoke), as its asset is named.
constexpr char star_texture[] = "UI/Textures/comMon/Shapes/Bespoke/img_Star_Fill_64";

std::uint32_t schema_of(md::Address type) { return type ? md::read<std::uint32_t>(md::read<md::Address>(type)) : 0; }

unsigned inline_offset(const md::Context& context, md::Address type, std::span<const std::uint32_t> hashes,
                       md::Address* last = nullptr) {
    unsigned offset = 0;
    for (const auto hash : hashes) {
        const auto field = context.member(type, hash);
        offset += field.offset;
        type = field.type;
    }
    if (last) *last = type;
    return offset;
}

// The favorites file: beside Skate.exe, where the launcher keeps its own settings.
struct Favorites {
    std::set<std::string, std::less<>> keys;
    std::filesystem::path file;
    bool loaded{};

    void load() {
        if (loaded) return;
        loaded = true;
        std::array<wchar_t, 32768> buffer{};
        if (const auto length = GetEnvironmentVariableW(L"RESKATE_FAVORITES_FILE", buffer.data(), static_cast<DWORD>(buffer.size()));
            length && length < buffer.size()) {
            file = std::wstring(buffer.data(), length);
        } else {
            const auto count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (!count || count >= buffer.size()) return;
            file = std::filesystem::path(std::wstring(buffer.data(), count)).parent_path() / L"ReSkate.favorites.json";
        }
        try {
            std::ifstream in(file, std::ios::binary);
            if (!in) return;
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            const auto json = Json::parse(text);
            if (json.is_object() && json.contains("favorites") && json.at("favorites").is_array())
                for (std::size_t i = 0; i < json.at("favorites").size(); ++i)
                    if (json.at("favorites").at(i).is_string()) keys.insert(json.at("favorites").at(i).string());
            logging::log(Level::info, Channel::customization, "Item favorites: {} loaded.", keys.size());
        } catch (const std::exception& error) {
            logging::log(Level::warning, Channel::customization, "Item favorites could not be read ({}); starting empty.", error.what());
        }
    }
    void save() const {
        if (file.empty()) return;
        try {
            Json json = Json::object();
            json["version"] = 1;
            json["favorites"] = Json::array();
            for (const auto& key : keys) json["favorites"].push_back(key);
            auto temporary = file;
            temporary += L".tmp";
            { std::ofstream out(temporary, std::ios::binary | std::ios::trunc); out << json.dump(2) << '\n'; }
            if (!MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("Windows error " + std::to_string(GetLastError()));
        } catch (const std::exception& error) {
            logging::log(Level::warning, Channel::customization, "Item favorites could not be saved: {}.", error.what());
        }
    }
};

struct Layout {
    md::Address row_type{};
    unsigned stride{}, content{}, focused{};
};

// One item grid of the game (a slot's tab), and our copy of every row it was built with.
struct Grid {
    md::Value list;    // the game's GridListViewModel root
    md::Value items;   // its OwnedItems
    md::Value backing; // ours: every row in the game's order, so a search can give rows back
    std::vector<Item> native;            // what the backing rows are
    std::vector<std::uint64_t> shown;    // item models of the rows the game's list has now
    int focused{-1};                     // row in `shown`
    bool failed{};
    std::string_view kind;               // the type tab picked (one of kind_names), empty: all
    std::vector<KindCount> kinds;        // the type tabs its items make, from the last sync
    Slot slot{};                         // what its items are, from the last list the game built
    // A grid keeps its focused tile after its screen has gone, so a focused tile alone does not say the
    // grid is on screen. The game moves the focus only in the grid that is: the newest move tells.
    ULONGLONG focus_moved{};             // when the game last moved the focus in it (to it, or to another tile)
    ULONGLONG unfocused_for{};           // the move after which its focus mark was taken off, as stale
    ULONGLONG last_shown{};              // when it last was the grid the overlay was for
};

struct State {
    md::MenuLifetime lifetime;
    md::Address manager{};
    Layout layout;
    std::vector<Grid> grids;
    // A favorite's tile shows a star where its rarity icon is: its item model points at our copy of its
    // rarity style, which has the star for an icon. One copy per rarity style, made when first needed.
    std::map<std::uint64_t, md::Value> starred;        // the game's style -> our starred copy
    std::map<std::uint64_t, std::uint64_t> restyled;   // item model -> the game's style it had
    bool star_unavailable{};
    unsigned tick{};
    Favorites favorites;
    std::string search;
    Filter filter{};
    ULONGLONG next_sync{}, next_scan{}, last_open{};
    std::uint64_t active_list{};  // the grid list that had the focus at the last full pass
    std::vector<std::uint64_t> notices;  // the game's notice models, as of the last scan
    std::uint64_t notice_info{};         // the info sign's texture, as last put on a notice
    ULONGLONG notice_info_retry{};       // not loaded: when to look for it again
    bool notice_cleared_logged{}, notice_failed_logged{}, notice_kept_logged{};
    std::uint64_t serial{};
    bool open{};
    std::string last_error;
};
State& state() { static auto* value = new State; return *value; }

struct Shared {
    std::mutex mutex;
    View view;
    std::string search;
    bool search_pending{};
    std::vector<std::string> pokes;
    std::atomic<unsigned> toggles{}, cycles{};
    std::atomic<int> filter{-1};
    std::atomic<int> kind_steps{}, kind_pick{-1}; // type tabs: steps right (left when negative), a tab picked
    std::atomic<bool> status{};
};
Shared& shared() { static auto* value = new Shared; return *value; }

void report(State& s, const std::string& what) {
    if (what == s.last_error) return;
    s.last_error = what;
    logging::log(Level::warning, Channel::customization, "Item browser: {}", what);
}

Layout layout_of(const md::Context& context, md::Value items) {
    Layout layout;
    layout.row_type = md::read<md::Address>(md::read<md::Address>(items.type) + 0x30);
    md::require(schema_of(layout.row_type) == tile_row, "Item grid rows are not tiles.");
    layout.stride = md::size(layout.row_type);
    md::Address last{};
    layout.content = inline_offset(context, layout.row_type, content_path, &last);
    md::require(schema_of(last) == reference && md::size(last) == sizeof(md::Ref), "Item tile content differs.");
    layout.focused = inline_offset(context, layout.row_type, focused_path, &last);
    md::require(md::size(last) == 1, "Item tile focus differs.");
    return layout;
}

struct Rows {
    std::vector<std::byte> bytes;
    unsigned count{};
    std::vector<std::uint64_t> content;
    int focused{-1};
};
Rows read_rows(const md::Context& context, md::Value items, const Layout& layout) {
    Rows rows;
    unsigned stride{};
    rows.bytes = context.array(items, 4096, rows.count, stride);
    md::require(!rows.count || stride == layout.stride, "Item grid row size differs.");
    rows.content.reserve(rows.count);
    for (unsigned i = 0; i < rows.count; ++i) {
        const auto row = rows.bytes.data() + std::size_t{i} * stride;
        md::Ref ref{};
        std::memcpy(&ref, row + layout.content, sizeof(ref));
        rows.content.push_back(ref.handle & ~std::uint64_t{1});
        if (rows.focused < 0 && row[layout.focused] != std::byte{0}) rows.focused = static_cast<int>(i);
    }
    return rows;
}

// The catalog item behind a tile's item model. Empty key: the tile does not show a cosmetic.
Item resolve(const md::Context& context, std::uint64_t content) {
    Item item;
    item.content = content;
    if (!content) return item;
    const md::Value view{content, context.type_of(content)};
    if (schema_of(view.type) != ownable_view) return item;
    const auto ref = md::read<md::Ref>(context.address(view) + context.member(view.type, ownable_data_ref).offset);
    const auto handle = ref.handle & ~std::uint64_t{1};
    const md::Value data{handle, handle ? context.type_of(handle) : 0};
    if (schema_of(data.type) != ownable_data) return item;
    const auto at = context.address(data);
    item.key = md::string(md::read<md::Address>(at + context.member(data.type, data_key).offset), 255);
    item.title = md::string(md::read<md::Address>(at + context.member(data.type, data_title).offset), 255);
    item.category = md::string(md::read<md::Address>(at + context.member(data.type, data_category).offset), 96);
    if (const auto& known = profile_runtime::cosmetic_runtime().items; !item.key.empty())
        if (const auto found = known.find(item.key); found != known.end()) item.shape = found->second.shape;
    const auto& catalogs = content_cache::catalogs();
    item.modded = catalogs.available && !catalogs.items.contains(folded(item.key));
    return item;
}

bool cosmetic(const Item& item) {
    if (item.key.empty()) return false;
    const auto& items = profile_runtime::cosmetic_runtime().items;
    const auto found = items.find(item.key);
    return found != items.end() && !found->second.build_kit;
}

void release(const md::Context& context, Grid& grid) {
    if (grid.backing.handle) {
        try { context.destroy(grid.backing); } catch (const std::exception&) {}
        grid.backing = {};
    }
}

std::uint64_t starred_source(const State& s, std::uint64_t style) {
    for (const auto& [source, copy] : s.starred) if (copy.handle == style) return source;
    return 0;
}

// Gives every favorite in the grid its star and takes it from the others. The game sets an item's style
// when its tile is first shown (and again when the tile comes back into view), so this repeats.
void mark(State& s, const md::Context& context, const Grid& grid) {
    if (s.star_unavailable) return;
    for (const auto& item : grid.native) {
        const md::Value view{item.content, context.type_of(item.content)};
        if (schema_of(view.type) != ownable_view) continue;
        const auto at = context.address(view) + inline_offset(context, view.type, rarity_path);
        const auto current = md::read<md::Ref>(at).handle & ~std::uint64_t{1};
        const auto source = starred_source(s, current);
        if (s.favorites.keys.contains(item.key)) {
            if (!current || source) continue;
            auto copy = s.starred.find(current);
            if (copy == s.starred.end()) {
                const md::Value style{current, context.type_of(current)};
                if (schema_of(style.type) != rarity_style.hash) continue;
                const auto made = context.create(rarity_style, (GetTickCount64() << 12) | (++s.serial & 0xfff));
                copy = s.starred.emplace(current, made).first;
                context.copy(made, context.address(style));
                if (!md::publish_texture(context, context.field(made, rarity_icon), star_texture)) {
                    s.star_unavailable = true;
                    logging::log(Level::warning, Channel::customization,
                        "Item browser: the game's star icon is not loaded here; favorites show no star.");
                    return;
                }
            }
            context.set(context.path(view, {rarity_path[0], rarity_path[1]}), md::Ref{0, copy->second.handle});
            s.restyled[item.content] = current;
        } else if (source) {
            context.set(context.path(view, {rarity_path[0], rarity_path[1]}), md::Ref{0, source});
            s.restyled.erase(item.content);
        }
    }
}

// Our style copies go when the screens that used them have: nothing may point at a destroyed model.
void release_styles(State& s, const md::Context& context) {
    for (const auto& [content, style] : s.restyled) {
        try {
            const md::Value view{content, context.type_of(content)};
            if (schema_of(view.type) == ownable_view)
                context.set(context.path(view, {rarity_path[0], rarity_path[1]}), md::Ref{0, style});
        } catch (const std::exception&) {}
    }
    s.restyled.clear();
    for (auto& [source, copy] : s.starred) {
        try { context.destroy(copy); } catch (const std::exception&) {}
    }
    s.starred.clear();
}

// Finds the game's item grids: every grid list one of whose first tiles shows a cosmetic (the
// accessory grids start with a "none" tile, which shows no item).
void scan(State& s, const md::Context& context) {
    std::erase_if(s.grids, [&](Grid& grid) {
        if (context.type_of(grid.list.handle) == grid.list.type) return false;
        release(context, grid); // the game closed the screen this grid was on
        return true;
    });
    if (s.grids.empty() && !s.starred.empty()) release_styles(s, context);
    for (const auto& root : context.roots({grid_list.hash})) {
        if (std::any_of(s.grids.begin(), s.grids.end(), [&](const Grid& grid) {
                return grid.list.handle == root.model.handle || grid.backing.handle == root.model.handle; })) continue;
        try {
            if (md::size(root.model.type) != grid_list.size) continue;
            Grid grid;
            grid.list = root.model;
            grid.items = context.field(grid.list, owned_items);
            if (!s.layout.row_type) s.layout = layout_of(context, grid.items);
            const auto rows = read_rows(context, grid.items, s.layout);
            bool items = false;
            for (unsigned i = 0; i < rows.count && i < 3 && !items; ++i) items = cosmetic(resolve(context, rows.content[i]));
            if (!items) continue;
            s.grids.push_back(std::move(grid));
        } catch (const std::exception&) { /* Another screen's grid list. */ }
    }
}

std::vector<std::byte> take_rows(const std::vector<std::byte>& from, const std::vector<unsigned>& order, const Layout& layout,
                                 int focused) {
    std::vector<std::byte> out;
    out.reserve(order.size() * layout.stride);
    for (unsigned position = 0; position < order.size(); ++position) {
        const auto row = from.data() + std::size_t{order[position]} * layout.stride;
        out.insert(out.end(), row, row + layout.stride);
        // The copy we keep recorded the focus as it was when the game built the list.
        out[out.size() - layout.stride + layout.focused] = static_cast<std::byte>(static_cast<int>(position) == focused);
    }
    return out;
}

// The research log of what the grids did is kept short: a session's first lines tell enough.
bool budget(unsigned& left) {
    if (!left) return false;
    --left;
    return true;
}

// The game changed the rows of a grid while a search, filter or type tab hid some of them. When those
// rows are only what was shown (keep_hidden), the game sorted or made again our shorter list: the hidden
// rows are put back beside the game's, so the grid does not lose them until the next level. False: the
// rows are the game's own whole list.
bool restore_hidden(State& s, const md::Context& context, Grid& grid, const Rows& rows, std::vector<Item>& items) {
    const auto merged = keep_hidden(grid.native, grid.shown, items);
    if (merged.empty()) return false;
    unsigned count{}, stride{};
    const auto backing = context.array(context.field(grid.backing, owned_items), 4096, count, stride);
    md::require(count == grid.native.size() && stride == s.layout.stride, "Item grid copy differs.");
    std::map<std::string_view, std::string_view> kinds_by_key; // the kinds the whole list gave its items
    for (const auto& item : grid.native) if (!item.key.empty()) kinds_by_key.emplace(item.key, item.kind);
    std::vector<Item> native;
    std::vector<std::byte> bytes;
    native.reserve(merged.size());
    bytes.reserve(merged.size() * stride);
    unsigned kept = 0, gone = 0;
    for (const auto& row : merged) {
        const std::byte* at{};
        Item item;
        if (row.from_game) {
            item = items[row.index];
            if (const auto found = kinds_by_key.find(item.key); found != kinds_by_key.end()) item.kind = found->second;
            at = rows.bytes.data() + std::size_t{row.index} * stride;
        } else {
            item = grid.native[row.index];
            // A hidden tile whose model went with the ones the game made again cannot be shown any more.
            if (!item.key.empty() && resolve(context, item.content).key != item.key) { ++gone; continue; }
            if (!item.key.empty()) ++kept;
            at = backing.data() + std::size_t{row.index} * stride;
        }
        native.push_back(std::move(item));
        bytes.insert(bytes.end(), at, at + stride);
    }
    static unsigned lines = 40;
    if (budget(lines))
        logging::log(Level::info, Channel::customization,
            "Item browser: the game remade the {} row(s) shown of a grid of slot {} ({} items); {} hidden item(s) put back{}.",
            rows.count, static_cast<int>(grid.slot), grid.native.size(), kept,
            gone ? ", " + std::to_string(gone) + " gone with the game's old tiles" : std::string{});
    if (!kept) return false;
    context.array(context.field(grid.backing, owned_items), bytes, static_cast<unsigned>(native.size()));
    grid.native = std::move(native);
    grid.shown = rows.content;
    return true;
}

// Takes the focus mark off the tiles of grids the focus has left: once the game has moved the focus in
// another grid, theirs is what their screen had when it went. Coming back to such a grid then shows as a
// move again, whichever tile the game puts the focus on.
void unmark_stale_focus(const md::Context& context, std::vector<Grid>& grids) {
    const Grid* newest = nullptr;
    for (const auto& grid : grids)
        if (grid.focused >= 0 && (!newest || grid.focus_moved > newest->focus_moved)) newest = &grid;
    if (!newest || !newest->focus_moved) return;
    for (auto& grid : grids) {
        if (&grid == newest || grid.focused < 0 || grid.focus_moved >= newest->focus_moved ||
            grid.unfocused_for == newest->focus_moved || static_cast<std::size_t>(grid.focused) >= grid.shown.size()) continue;
        grid.unfocused_for = newest->focus_moved; // once: if the game keeps the mark, it is left alone
        try {
            const auto row = context.element(grid.items, static_cast<unsigned>(grid.focused));
            context.set(context.path(row, {focused_path[0], focused_path[1], focused_path[2], focused_path[3], focused_path[4]}),
                        std::uint8_t{0});
        } catch (const std::exception&) {}
    }
}

std::string hex(std::uint64_t value, int width);
// fit21 research: which of the game's models names the item a grid FOCUSED (see patch_fit21.py). A few probes a
// session, only for grids of our accessories; read only.
void probe_focus(const md::Context& context, std::uint64_t content) {
    static unsigned left = 12;
    if (!left || !content) return;
    const auto item = resolve(context, content);
    if (item.key.empty() || !accessory_variant(item.key)) return;
    --left;
    const auto started = GetTickCount64();
    std::uint64_t data{}, view_at{}, data_at{};
    try {
        const md::Value view{content, context.type_of(content)};
        view_at = context.address(view);
        data = md::read<md::Ref>(view_at + context.member(view.type, ownable_data_ref).offset).handle & ~std::uint64_t{1};
        if (data) data_at = context.address(md::Value{data, context.type_of(data)});
    } catch (...) {}
    // fit25: what a value names, if it is one of them
    const auto names = [&](std::uint64_t v) -> const char* {
        if (!v) return nullptr;
        const auto h = v & ~std::uint64_t{1};
        if (h == content) return " -> the tile's item model";
        if (h == data) return " -> its catalog item";
        if (v == view_at) return " -> the tile's item model (address)";
        if (v == data_at) return " -> its catalog item (address)";
        return nullptr;
    };
    std::string found;
    unsigned roots_n = 0, fields_n = 0, hits = 0;
    std::set<std::uint64_t> seen;
    std::function<void(md::Address, md::Address, const std::string&, int)> walk =
        [&](md::Address type, md::Address at, const std::string& path, int depth) {
        if (fields_n > 3000000 || hits > 40) return;
        const auto meta = md::read<md::Address>(type), fields = md::read<md::Address>(meta + 0x60);
        const auto count = md::read<std::uint16_t>(meta + 0x2a);
        for (unsigned i = 0; i < count && i < 256; ++i) {
            const auto entry = fields + i * 24;
            const auto hash = md::read<std::uint32_t>(entry);
            const auto ftype = md::read<md::Address>(entry + 16);
            const auto offset = md::read<std::uint16_t>(entry + 8);
            ++fields_n;
            if (!ftype) continue;
            const auto kind = md::kind(ftype);
            const auto here = path + "." + hex(hash, 8);
            const auto fsize = md::size(ftype);
            if (fsize == 8 && kind != 7) {
                if (const auto* what = names(md::read<std::uint64_t>(at + offset)))
                    ++hits, found += "\n    " + here + " (8 bytes, schema " + hex(schema_of(ftype), 8) + ")" + what;
                continue;
            }
            if (fsize == sizeof(md::Ref)) {
                // any 16-byte value: a DataModelReference or another typed reference (UIOwnableDataRef's own kind)
                const auto ref = md::read<md::Ref>(at + offset);
                if (const auto* what = names(ref.handle)) {
                    ++hits, found += "\n    " + here + " (schema " + hex(schema_of(ftype), 8) + ")" + what;
                    continue;
                }
            }
            if (schema_of(ftype) == reference && fsize == sizeof(md::Ref)) {
                const auto h = md::read<md::Ref>(at + offset).handle & ~std::uint64_t{1};
                if (h && depth < 3 && seen.insert(h).second) {
                    try {
                        const md::Value next{h, context.type_of(h)};
                        if (next.type && md::kind(next.type) == 2) walk(next.type, context.address(next), here + ">", depth + 1);
                    } catch (...) {}
                }
            } else if (kind == 7) {
                try {
                    if (md::string(md::read<md::Address>(at + offset), 96) == item.key) ++hits, found += "\n    " + here + " = its key";
                } catch (...) {}
            } else if (kind == 2 && depth < 4) {
                walk(ftype, at + offset, here, depth + 1);
            }
        }
    };
    for (const auto& root : context.roots()) {
        ++roots_n;
        if (schema_of(root.model.type) == grid_list.hash) continue; // the grids' own rows name it, of course
        if (!seen.insert(root.model.handle).second) continue;
        try { walk(root.model.type, context.address(root.model), hex(schema_of(root.model.type), 8) + "@" + hex(root.model.handle, 12), 0); }
        catch (...) {}
    }
    logging::log(Level::info, Channel::customization,
        "Item browser: focus probe on {} (tile item model {}, catalog item {}): {} roots, {} fields, {} ms{}", item.key,
        hex(content, 12), hex(data, 12), roots_n, fields_n, GetTickCount64() - started,
        found.empty() ? std::string(" - nothing refers to them") : found);
}

// fit28 research: the game's "focused item" is in no data model (fit25): the heap. After the focus moves onto one of
// our tiles, a background scan lists the 8-byte slots holding that tile's item model / catalog item (handle, handle | 1,
// native address); every later scan keeps only the slots that now hold the NEW tile's, by kind. Read only.
namespace focus_scan {
struct Want { std::uint64_t value{}; std::uint8_t kind{}; };
struct Hit { std::uintptr_t at{}; std::uint8_t kind{}; };
constexpr const char* kind_names[]{"item model", "item model|1", "catalog item", "catalog item|1", "item model address",
                                   "catalog item address"};
constexpr std::size_t most = 1u << 20;
struct Shared {
    std::mutex lock;
    std::atomic<bool> busy{};
    std::atomic<std::uint64_t> serial{};   // focus moves seen
    std::vector<Hit> kept;
    unsigned rounds{};
};
Shared& shared() { static auto* value = new Shared; return *value; }
std::uint64_t scan_range(const std::uint8_t* p, std::size_t n, std::uintptr_t start, const Want* want, std::size_t wants,
                         const std::uint8_t* first, Hit* out, std::size_t& count) noexcept {
    __try {
        for (std::size_t i = 0; i + 8 <= n; i += 8) {
            const auto v = *reinterpret_cast<const std::uint64_t*>(p + i);
            if (!first[v & 0xffff]) continue;
            for (std::size_t k = 0; k < wants; ++k)
                if (v == want[k].value) {
                    if (count < most) out[count++] = {start + i, want[k].kind};
                    break;
                }
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 1; }
}
void run(std::vector<Want> want, std::string key, std::uint64_t serial) {
    auto& s = shared();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const auto began = GetTickCount64();
    std::vector<std::uint8_t> first(65536);
    for (const auto& w : want) first[w.value & 0xffff] = 1;
    std::vector<Hit> hits(most);
    std::size_t count{};
    std::uint64_t faults{}, bytes{};
    MEMORY_BASIC_INFORMATION info{};
    for (std::uintptr_t at = 0x10000; at < memory::highest_user_address;) {
        if (!VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info))) break;
        const auto start = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        const auto size = static_cast<std::size_t>(info.RegionSize);
        at = start + size;
        if (info.State != MEM_COMMIT || (info.Type != MEM_PRIVATE && info.Type != MEM_IMAGE) ||
            (info.Protect & (PAGE_GUARD | PAGE_NOACCESS | PAGE_NOCACHE | PAGE_WRITECOMBINE)))
            continue;
        const DWORD kind = info.Protect & 0xff;
        if (kind != PAGE_READWRITE && kind != PAGE_EXECUTE_READWRITE && kind != PAGE_WRITECOPY) continue;
        faults += scan_range(reinterpret_cast<const std::uint8_t*>(start), size, start, want.data(), want.size(), first.data(),
                             hits.data(), count);
        bytes += size;
    }
    hits.resize(count);
    std::string line;
    {
        std::lock_guard guard(s.lock);
        if (s.serial.load() != serial) {
            line = std::format("Item browser: focus scan on {} discarded (the focus moved during it)", key);
        } else {
            if (!s.rounds++) {
                s.kept = hits;
            } else {
                std::vector<Hit> next;
                std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.at < b.at; });
                for (const auto& k : s.kept) {
                    const auto it = std::lower_bound(hits.begin(), hits.end(), k.at, [](const Hit& h, std::uintptr_t a) { return h.at < a; });
                    if (it != hits.end() && it->at == k.at && it->kind == k.kind) next.push_back(k);
                }
                s.kept = std::move(next);
            }
            line = std::format("Item browser: focus scan {} on {}: {} slot(s) hold it, {} kept since the first scan; {:.1f} GB, {} ms, {} fault(s)",
                               s.rounds, key, count, s.kept.size(), static_cast<double>(bytes) / (1u << 30), GetTickCount64() - began, faults);
            if (s.rounds > 1 && s.kept.size() <= 40)
                for (const auto& k : s.kept) {
                    std::uint64_t before{}, value{}, after{};
                    (void)memory::peek(k.at - 8, before), (void)memory::peek(k.at, value), (void)memory::peek(k.at + 8, after);
                    MEMORY_BASIC_INFORMATION where{};
                    VirtualQuery(reinterpret_cast<void*>(k.at), &where, sizeof(where));
                    line += std::format("\n    {:#x} ({}; region {:#x} +{:#x}, {}) = {:#x} [{:#x} | {:#x}]", k.at, kind_names[k.kind],
                                        reinterpret_cast<std::uintptr_t>(where.AllocationBase),
                                        k.at - reinterpret_cast<std::uintptr_t>(where.AllocationBase),
                                        where.Type == MEM_IMAGE ? "image" : "heap", value, before, after);
                }
        }
    }
    logging::log(Level::info, Channel::customization, "{}", line);
    s.busy.store(false);
}
// The focus moved onto `content` (one of our tiles): a scan, when none is running. Client thread.
void focused(const md::Context& context, std::uint64_t content, const std::string& key) {
    auto& s = shared();
    const auto serial = s.serial.fetch_add(1) + 1;
    if (s.rounds >= 8 || s.busy.exchange(true)) return;
    try {
        const md::Value view{content, context.type_of(content)};
        const auto view_at = context.address(view);
        const auto data = md::read<md::Ref>(view_at + context.member(view.type, ownable_data_ref).offset).handle & ~std::uint64_t{1};
        const auto data_at = data ? context.address(md::Value{data, context.type_of(data)}) : 0;
        std::vector<Want> want{{content, 0}, {content | 1, 1}, {data, 2}, {data | 1, 3}, {view_at, 4}, {data_at, 5}};
        std::erase_if(want, [](const Want& w) { return w.value < 0x10000; });
        std::thread(run, std::move(want), key, serial).detach();
    } catch (...) {
        s.busy.store(false);
    }
}
} // namespace focus_scan

void sync(State& s, const md::Context& context, Grid& grid, bool verify) {
    if (grid.failed) return;
    grid.items = context.field(grid.list, owned_items);
    auto rows = read_rows(context, grid.items, s.layout);
    if (rows.focused >= 0 && rows.focused != grid.focused) {
        grid.focus_moved = GetTickCount64();
        if (grid.focused >= 0) try { probe_focus(context, rows.content[static_cast<std::size_t>(rows.focused)]); } catch (...) {}
        // fit28: the heap scan for the game's focused item (our tiles only)
        try {
            const auto content = rows.content[static_cast<std::size_t>(rows.focused)];
            if (const auto item = resolve(context, content); !item.key.empty() && accessory_variant(item.key))
                focus_scan::focused(context, content, item.key);
        } catch (...) {}
    }
    grid.focused = rows.focused;
    if (rows.content != grid.shown || verify) {
        // Not what we last put there: the game built the list again (the screen opened, an item was
        // equipped, mods changed). Its list is complete and in its own order, so it is the new source --
        // unless it only sorted or remade the shorter list it was shown (restore_hidden).
        std::vector<Item> items;
        items.reserve(rows.count);
        for (const auto content : rows.content) items.push_back(resolve(context, content));
        bool ours = rows.content == grid.shown && !grid.native.empty();
        if (ours) {
            // Same item models as we published: check they still show the same items.
            for (std::size_t i = 0; i < items.size() && ours; ++i) {
                const auto known = std::find_if(grid.native.begin(), grid.native.end(),
                    [&](const Item& item) { return item.content == items[i].content; });
                ours = known != grid.native.end() && known->key == items[i].key;
            }
        }
        if (!ours) assign_kinds(items);
        if (!ours && !restore_hidden(s, context, grid, rows, items)) {
            // A whole list shorter than the one it replaces: worth knowing when a grid seems to lose items.
            static unsigned shorter_lines = 40;
            if (!grid.native.empty() && items.size() < grid.native.size() && budget(shorter_lines))
                logging::log(Level::info, Channel::customization, "Item browser: a grid of slot {} now has {} rows ({} before).",
                    static_cast<int>(grid.slot), items.size(), grid.native.size());
            grid.slot = grid_slot(items);
            // Once per grid as built: how its items were sorted, and what was left as "Other" (for the rules).
            static std::map<std::pair<int, std::size_t>, bool> logged;
            if (const auto slot = static_cast<int>(grid.slot); slot && !logged[{slot, items.size()}]) {
                logged[{slot, items.size()}] = true;
                std::map<std::string_view, unsigned> counts;
                std::string others;
                unsigned shaped = 0, listed = 0;
                for (const auto& item : items) {
                    if (item.key.empty()) continue;
                    ++counts[item.kind];
                    shaped += !item.shape.empty();
                    if (item.kind == "Other" && listed++ < 40) others += "\n    " + item.key + " | " + item.title + " | " + item.shape;
                }
                std::string line;
                for (const auto& [kind, count] : counts) line += " " + std::string(kind) + " " + std::to_string(count) + ",";
                logging::log(Level::info, Channel::customization, "Item browser: grid of slot {} ({} items, {} with a shape):{}{}",
                    slot, items.size(), shaped, line, others);
            }
            if (!grid.backing.handle)
                grid.backing = context.create(grid_list, (GetTickCount64() << 12) | (++s.serial & 0xfff));
            context.array(context.field(grid.backing, owned_items), rows.bytes, rows.count);
            grid.native = std::move(items);
            grid.shown = rows.content;
        }
    }
    const auto picks = accessory_probe::model_picks();
    grid.kinds = kinds(grid.native, s.favorites.keys, s.search, s.filter, &picks);
    // Public build (forestmouse, 2026-10-22): type tabs only over the accessories (Lanyards / Keychains / Chains);
    // every other grid stays as ReSkate shows it
    if (std::none_of(grid.kinds.begin(), grid.kinds.end(), [](const KindCount& k) {
            return k.name == "Lanyards" || k.name == "Keychains" || k.name == "Chains";
        }))
        grid.kinds.clear();
    // A list built again for another slot has other kinds: its tabs start from "all".
    if (!grid.kind.empty() && std::none_of(grid.kinds.begin(), grid.kinds.end(), [&](const KindCount& k) { return k.name == grid.kind; }))
        grid.kind = {};
    const auto order = arrange(grid.native, s.favorites.keys, s.search, s.filter, grid.kind, &picks);
    std::vector<std::uint64_t> wanted;
    wanted.reserve(order.size());
    for (const auto index : order) wanted.push_back(grid.native[index].content);
    if (wanted == grid.shown) return;
    unsigned count{}, stride{};
    const auto source = context.array(context.field(grid.backing, owned_items), 4096, count, stride);
    md::require(count == grid.native.size() && stride == s.layout.stride, "Item grid copy differs.");
    const auto focused = grid.focused < 0 ? -1 : std::min(grid.focused, static_cast<int>(order.size()) - 1);
    context.array(grid.items, take_rows(source, order, s.layout, focused), static_cast<unsigned>(order.size()));
    const auto after = read_rows(context, grid.items, s.layout);
    if (after.content != wanted) {
        grid.failed = true;
        throw std::runtime_error("the game did not take the reordered tiles; this grid is left alone.");
    }
    grid.shown = std::move(wanted);
    grid.focused = after.focused;
}

md::Value deref(const md::Context& context, md::Value value) {
    md::require(schema_of(value.type) == reference, "Not a model reference.");
    const auto handle = md::read<md::Ref>(context.address(value)).handle & ~std::uint64_t{1};
    md::require(handle != 0, "The reference is empty.");
    return {handle, context.type_of(handle)};
}

std::string hex(std::uint64_t value, int width) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out(static_cast<std::size_t>(width), '0');
    for (int i = width - 1; i >= 0; --i, value >>= 4) out[static_cast<std::size_t>(i)] = digits[value & 15];
    return "0x" + out;
}

// Research aid: "peek <root> [path]" and "poke <root> <path> <kind>:<value>" on live models.
// root: grid | row | item | data (of the focused tile) or a model handle; path: field hashes, [n] and
// ">" (follow a reference), separated by dots.
std::string poke(State& s, const md::Context& context, const std::string& arguments) {
    std::vector<std::string> words;
    for (std::size_t at = 0; at < arguments.size();) {
        const auto end = std::min(arguments.find(' ', at), arguments.size());
        if (end > at) words.push_back(arguments.substr(at, end - at));
        at = end + 1;
        if (words.size() == 3 && at < arguments.size()) { words.push_back(arguments.substr(at)); break; }
    }
    md::require(words.size() >= 2, "usage: peek <root> [path] | poke <root> <path> <kind>:<value>");
    const bool write = words[0] == "poke";
    const auto focused = std::find_if(s.grids.begin(), s.grids.end(), [](const Grid& grid) { return grid.focused >= 0; });
    md::Value value;
    if (words[1].starts_with("0x")) {
        const auto handle = std::stoull(words[1].substr(2), nullptr, 16);
        value = {handle, context.type_of(handle)};
    } else {
        md::require(focused != s.grids.end(), "No item tile has the focus.");
        if (words[1] == "grid") value = focused->list;
        else if (words[1] == "back") value = focused->backing;
        else {
            value = context.element(focused->items, static_cast<unsigned>(focused->focused));
            if (words[1] != "row") {
                value = deref(context, context.path(value, {content_path[0], content_path[1]}));
                if (words[1] == "data") value = deref(context, context.field(value, ownable_data_ref));
                else md::require(words[1] == "item", "root: grid, back, row, item, data or a 0x handle");
            }
        }
    }
    md::require(value.handle && value.type, "That model does not exist.");
    const std::string path = words.size() > 2 && (write || words.size() == 3) ? words[2] : std::string{};
    for (std::size_t at = 0; at < path.size();) {
        const auto end = std::min(path.find('.', at), path.size());
        const auto token = path.substr(at, end - at);
        at = end + 1;
        if (token.empty()) continue;
        if (token == ">") value = deref(context, value);
        else if (token.front() == '[') value = context.element(value, static_cast<unsigned>(std::stoul(token.substr(1))));
        else value = context.field(value, static_cast<std::uint32_t>(std::stoul(token, nullptr, 16)));
    }
    const auto kind = md::kind(value.type), size = md::size(value.type);
    if (write) {
        md::require(words.size() == 4, "poke needs <kind>:<value>");
        const auto colon = words[3].find(':');
        md::require(colon != std::string::npos, "poke needs <kind>:<value>");
        const auto how = words[3].substr(0, colon), text = words[3].substr(colon + 1);
        if (how == "u8") context.set(value, static_cast<std::uint8_t>(std::stoul(text, nullptr, 0)));
        else if (how == "u32") context.set(value, static_cast<std::uint32_t>(std::stoul(text, nullptr, 0)));
        else if (how == "f32") context.set(value, std::stof(text));
        else if (how == "text") context.text(value, text);
        else if (how == "ref") context.set(value, md::Ref{0, std::stoull(text, nullptr, 0)});
        else if (how == "tex") md::require(md::publish_texture(context, value, text), "That texture is not loaded.");
        else throw std::runtime_error("kinds: u8, u32, f32, text, ref, tex");
    }
    std::string out = (write ? "poked " : "peek ") + hex(value.handle, 16) + " <" + hex(schema_of(value.type), 8) + " k" +
        std::to_string(kind) + ' ' + std::to_string(size) + ">";
    const auto at = context.address(value);
    if (kind == 7) out += " = \"" + md::string(md::read<md::Address>(at), 255) + '"';
    else if (kind == 4) {
        const auto pointer = md::read<md::Address>(at);
        out += " [" + std::to_string(pointer ? md::read<std::uint32_t>(pointer - 4) & 0x7fffffff : 0) + "] of <" +
            hex(schema_of(md::read<md::Address>(md::read<md::Address>(value.type) + 0x30)), 8) + ">";
    } else if (kind == 2) {
        const auto meta = md::read<md::Address>(value.type), fields = md::read<md::Address>(meta + 0x60);
        const auto count = md::read<std::uint16_t>(meta + 0x2a);
        for (unsigned i = 0; i < count && i < 64; ++i) {
            const auto entry = fields + i * 24;
            const auto type = md::read<md::Address>(entry + 16);
            const auto offset = md::read<std::uint16_t>(entry + 8);
            out += "\n    " + hex(md::read<std::uint32_t>(entry), 8) + '@' + std::to_string(offset) + " <" +
                hex(schema_of(type), 8) + " k" + std::to_string(md::kind(type)) + ' ' + std::to_string(md::size(type)) + ">";
            if (md::kind(type) == 7) out += " \"" + md::string(md::read<md::Address>(at + offset), 96) + '"';
            else if (md::kind(type) != 2 && md::size(type) <= 16) {
                out += " ";
                for (unsigned b = 0; b < md::size(type); ++b) out += hex(md::read<std::uint8_t>(at + offset + b), 2).substr(2);
            }
        }
    } else {
        out += " = ";
        for (unsigned b = 0; b < std::min(size, 32U); ++b) out += hex(md::read<std::uint8_t>(at + b), 2).substr(2);
    }
    return out;
}

// The type tabs: "all", then the grid's kinds. A step skips the tabs the search and the filter leave
// empty, and goes round from the last to the first.
void step_kind(Grid& grid, int steps, int pick) {
    if (grid.kinds.size() < 2) return;
    const auto tabs = static_cast<int>(grid.kinds.size()) + 1;
    int tab = 0;
    for (int i = 0; i < tabs - 1; ++i) if (grid.kinds[static_cast<std::size_t>(i)].name == grid.kind) tab = i + 1;
    const auto usable = [&](int t) { return t == 0 || grid.kinds[static_cast<std::size_t>(t - 1)].shown > 0; };
    if (pick >= 0 && pick < tabs && usable(pick)) tab = pick;
    for (int left = std::abs(steps); left > 0; --left) {
        const int way = steps > 0 ? 1 : -1;
        do tab = (tab + way + tabs) % tabs; while (!usable(tab));
    }
    const std::string_view kind = tab ? grid.kinds[static_cast<std::size_t>(tab - 1)].name : std::string_view{};
    if (kind == grid.kind) return;
    grid.kind = kind;
    logging::log(Level::info, Channel::customization, "Item browser: type tab {}.", kind.empty() ? std::string_view("All") : kind);
}

// Research aid, with the status: every grid list the game has open, taken or not, and what its
// first tiles show. Says why a menu's grid was passed over.
void log_roots(State& s, const md::Context& context) {
    for (const auto& root : context.roots({grid_list.hash})) {
        std::string line = "  root " + hex(root.model.handle, 16) + ": size " + std::to_string(md::size(root.model.type));
        try {
            const auto items = context.field(root.model, owned_items);
            const auto row_type = md::read<md::Address>(md::read<md::Address>(items.type) + 0x30);
            line += ", rows of " + hex(schema_of(row_type), 8);
            if (schema_of(row_type) == tile_row) {
                if (!s.layout.row_type) s.layout = layout_of(context, items);
                const auto rows = read_rows(context, items, s.layout);
                line += ", " + std::to_string(rows.count) + " tile(s)";
                for (unsigned i = 0; i < rows.count && i < 4; ++i) {
                    const auto content = rows.content[i];
                    const auto item = resolve(context, content);
                    line += "; [" + std::to_string(i) + "] " + (content ? hex(schema_of(context.type_of(content)), 8) : std::string("-")) +
                            " \"" + item.key + "\" in \"" + item.category + "\"" + (cosmetic(item) ? "" : " (no cosmetic)");
                }
            }
        } catch (const std::exception& error) { line += std::string(": ") + error.what(); }
        logging::log(Level::info, Channel::customization, "{}", line);
    }
}
void log_status(const State& s) {
    logging::log(Level::info, Channel::customization, "Item browser: {} grid(s), search \"{}\", filter {}, {} favorite(s), file {}.",
        s.grids.size(), s.search, filter_names[static_cast<std::size_t>(s.filter)], s.favorites.keys.size(),
        path_utf8(s.favorites.file));
    for (const auto& grid : s.grids) {
        std::string focus = "-";
        if (grid.focused >= 0 && static_cast<std::size_t>(grid.focused) < grid.shown.size())
            for (const auto& item : grid.native)
                if (item.content == grid.shown[static_cast<std::size_t>(grid.focused)]) focus = std::to_string(grid.focused) + " " + item.key;
        std::string tabs;
        for (const auto& kind : grid.kinds) tabs += " " + std::string(kind.name) + " " + std::to_string(kind.total);
        const auto now = GetTickCount64();
        logging::log(Level::info, Channel::customization,
            "  grid {} (slot {}): {} of {} shown, first \"{}\", focus {} (moved {} ms ago, on screen {} ms ago){}; type {}, tabs:{}.",
            hex(grid.list.handle, 16), static_cast<int>(grid.slot), grid.shown.size(), grid.native.size(),
            grid.native.empty() ? std::string{} : grid.native.front().title, focus,
            grid.focus_moved ? now - grid.focus_moved : 0, grid.last_shown ? now - grid.last_shown : 0, grid.failed ? ", FAILED" : "",
            grid.kind.empty() ? std::string_view("All") : grid.kind, tabs);
    }
}
} // namespace

View view() {
    auto& sh = shared();
    std::lock_guard lock(sh.mutex);
    return sh.view;
}
void toggle_focused_favorite() noexcept { shared().toggles.fetch_add(1); }
void set_search(std::string text) {
    auto& sh = shared();
    std::lock_guard lock(sh.mutex);
    sh.search = std::move(text);
    sh.search_pending = true;
}
void set_filter(Filter filter) noexcept { shared().filter.store(static_cast<int>(filter)); }
const overlay::ItemBrowserHost* overlay_host() noexcept {
    static const overlay::ItemBrowserHost host{
        [] {
            const auto current = view();
            overlay::ItemBrowserView out;
            out.open = current.open;
            out.search = current.search;
            out.filter = std::string(filter_names[static_cast<std::size_t>(current.filter)]);
            out.shown = current.shown;
            out.total = current.total;
            out.focused_favorite = current.focused_favorite;
            out.focused_key = current.focused_key;
            out.focused_index = current.focused_index;
            out.focused_shows = current.focused_shows;
            out.rows = current.rows;
            // The mod the highlighted item is from, for its card (item_origins.h).
            if (const auto origin = current.focused_key.empty() ? nullptr : item_origins::find(current.focused_key)) {
                out.mod_title = origin->title;
                out.mod_author = origin->author;
                out.mod_version = origin->version;
                out.mod_icon = path_utf8(origin->icon);
                out.mod_items = static_cast<unsigned>(origin->items);
                out.mod_changed = item_origins::game_item(current.focused_key);
            }
            for (const auto& tab : current.tabs) out.tabs.push_back({std::string(tab.name), tab.shown, tab.total});
            out.tab = current.tab;
            return out;
        },
        [] { toggle_focused_favorite(); },
        [](const char* text) { set_search(text ? text : ""); },
        [] { cycle_filter(); },
        [](int steps) { step_tab(steps); },
        [](unsigned tab) { pick_tab(tab); }};
    return &host;
}
void step_tab(int steps) noexcept { shared().kind_steps.fetch_add(steps); }
void pick_tab(unsigned tab) noexcept { shared().kind_pick.store(static_cast<int>(tab)); }
void cycle_filter() noexcept { shared().cycles.fetch_add(1); }
void request_status() noexcept { shared().status.store(true); }
void request_poke(std::string arguments) {
    auto& sh = shared();
    std::lock_guard lock(sh.mutex);
    if (sh.pokes.size() < 32) sh.pokes.push_back(std::move(arguments));
}

void before_level_transition(unsigned next, std::uintptr_t base) noexcept {
    try {
        auto& s = state();
        s.lifetime.before_transition(next, [&] {
            // Our copies hold references to the screen's widgets and styles: let go while those are loaded.
            md::Address ui{}, manager{};
            if (memory::read(base + addr::engine::ui_manager, ui) && ui && memory::read(ui + 0x140, manager) &&
                manager && manager == s.manager) {
                game::ModelWriteLock lock(manager);
                const md::Context context(base, manager);
                for (auto& grid : s.grids) release(context, grid);
                release_styles(s, context);
            }
            s.grids.clear();
            s.starred.clear();
            s.restyled.clear();
            s.layout = {};
            s.open = false;
            s.notice_info = 0; // (its texture may go with the level)
        });
    } catch (...) {}
}

// A notice showing a mod item's reserved description has the info sign for the warning sign (see
// notice_view). Every tick while a grid is open: the game puts the warning back on each focus.
void quiet_notices(State& s, const md::Context& context) {
    for (const auto handle : s.notices) {
        try {
            const md::Value notice{handle, context.type_of(handle)};
            if (schema_of(notice.type) != notice_view) continue;
            // Only as much of its text as tells: the others' notices may say a lot.
            const auto text = context.path(notice, {notice_message, label_text});
            md::require(md::kind(text.type) == 7, "Native menu text field differs.");
            const auto& reserved = item_origins::reserved_description;
            std::array<char, item_origins::reserved_description.size() + 1> start{};
            const auto chars = md::read<md::Address>(context.address(text));
            if (!chars || !memory::peek_bytes(chars, start.data(), start.size()) || start.back() != 0 ||
                std::string_view(start.data(), reserved.size()) != reserved)
                continue;
            const auto icon = context.field(notice, notice_icon);
            std::uint64_t now{};
            md::require(md::size(icon.type) == sizeof(now), "Native notice icon field differs.");
            if (!memory::peek(context.address(icon), now) || !now || now == s.notice_info) continue;
            // A texture the game has loaded, or the warning stays: never an empty override.
            if (GetTickCount64() < s.notice_info_retry) continue;
            if (!md::publish_texture(context, icon, notice_info_texture)) {
                s.notice_info_retry = GetTickCount64() + 2000;
                if (!std::exchange(s.notice_kept_logged, true))
                    logging::log(Level::info, Channel::customization, "Item browser: the info sign is not loaded; a mod item's card keeps the warning sign.");
                continue;
            }
            std::uint64_t info{};
            if (memory::peek(context.address(icon), info)) s.notice_info = info;
            if (!std::exchange(s.notice_cleared_logged, true))
                logging::log(Level::info, Channel::customization, "Item browser: a mod item's card shows the info sign for the warning sign.");
        } catch (const std::exception& error) {
            if (!std::exchange(s.notice_failed_logged, true))
                logging::log(Level::warning, Channel::customization, "Item browser: a notice could not be checked for a mod item's warning sign: {}", error.what());
        }
    }
}

// Between full passes, only the highlight: which tile of the open grid has the focus, and whether
// its screen is still up. So what the overlay shows of the focused item (its mod's card) follows
// the player's moves at once instead of trailing the full pass by up to its interval.
void follow_focus(State& s, Shared& sh, std::uintptr_t base) {
    const auto grid = std::find_if(s.grids.begin(), s.grids.end(), [&](const Grid& g) { return g.list.handle == s.active_list; });
    if (grid == s.grids.end() || grid->failed || !s.layout.row_type) return;
    game::ModelWriteLock lock(s.manager);
    const md::Context context(base, s.manager);
    quiet_notices(s, context);
    View next;
    {
        std::lock_guard shared_lock(sh.mutex);
        next = sh.view;
    }
    if (context.type_of(grid->list.handle) != grid->list.type) {
        // The screen closed: nothing is open now (the next full pass lets the grid go).
        s.open = false;
        s.active_list = 0;
        next = {};
    } else {
        const auto rows = read_rows(context, context.field(grid->list, owned_items), s.layout);
        if (rows.focused < 0 || static_cast<std::size_t>(rows.focused) >= rows.content.size()) return;
        const auto content = rows.content[static_cast<std::size_t>(rows.focused)];
        const auto item = std::find_if(grid->native.begin(), grid->native.end(), [&](const Item& i) { return i.content == content; });
        if (item == grid->native.end()) return;
        if (item->key == next.focused_key) {
            // the same tile: what it shows can still have changed (an accessory tile pointed at another pick)
            auto shows = resolve(context, content).key;
            if (shows == next.focused_shows) return;
            next.focused_shows = std::move(shows);
            std::lock_guard shared_lock(sh.mutex);
            sh.view = std::move(next);
            return;
        }
        next.focused_title = item->title;
        next.focused_key = item->key;
        next.focused_favorite = s.favorites.keys.contains(item->key);
        next.focused_index = rows.focused;
        next.rows = rows.count;
        next.focused_shows = resolve(context, content).key;
    }
    std::lock_guard shared_lock(sh.mutex);
    sh.view = std::move(next);
}

void update(std::uintptr_t base) noexcept {
    auto& s = state();
    auto& sh = shared();
    try {
        if (s.lifetime.blocked()) return;
        const auto now = GetTickCount64();
        if (now < s.next_sync) {
            if (s.open && s.manager && s.active_list) follow_focus(s, sh, base);
            return;
        }
        s.next_sync = now + (s.open ? 40 : 200);
        s.favorites.load();
        const bool in_menu = multiplayer::sample_game_ui_state(base).in_menu;
        md::Address ui{}, manager{};
        if (!memory::read(base + addr::engine::ui_manager, ui) || !ui || !memory::read(ui + 0x140, manager) || !manager) return;
        if (manager != s.manager) {
            s.grids.clear(); s.starred.clear(); s.restyled.clear();
            s.layout = {}; s.manager = manager;
        }
        if (!in_menu && s.grids.empty() && !sh.status.load()) {
            std::lock_guard lock(sh.mutex);
            if (sh.pokes.empty()) { sh.view = {}; return; }
        }
        game::ModelWriteLock lock(manager);
        const md::Context context(base, manager);
        const bool rescan = now >= s.next_scan;
        if (rescan) {
            s.next_scan = now + 400;
            scan(s, context);
            s.notices.clear();
            if (!s.grids.empty())
                for (const auto& root : context.roots({notice_view})) s.notices.push_back(root.model.handle);
        }
        // Requests.
        std::vector<std::string> pokes;
        {
            std::lock_guard shared_lock(sh.mutex);
            if (std::exchange(sh.search_pending, false)) s.search = sh.search;
            pokes.swap(sh.pokes);
        }
        if (const auto filter = sh.filter.exchange(-1); filter >= 0 && filter <= static_cast<int>(Filter::game))
            s.filter = static_cast<Filter>(filter);
        for (auto cycles = sh.cycles.exchange(0); cycles; --cycles)
            s.filter = static_cast<Filter>((static_cast<unsigned>(s.filter) + 1) % std::size(filter_names));
        Grid* active = nullptr;
        const Item* focused = nullptr;
        // The grid on screen: of those with a focused tile, the one the game last moved the focus in.
        // (The first one found used to be taken, and that could be a grid of a screen that had gone, which
        // kept its focused tile: the overlay was for it, without tabs, while the grid on screen stayed on
        // a type tab no key could change until the next level.)
        const auto find_focus = [&] {
            active = nullptr; focused = nullptr;
            for (auto& grid : s.grids) {
                if (grid.focused < 0 || static_cast<std::size_t>(grid.focused) >= grid.shown.size()) continue;
                // Its screen closed: gone at the next scan, but no longer what the player is in now (upstream
                // 2.1.0), and of the rest the grid the game last moved the focus in (ours, tabs4).
                if (context.type_of(grid.list.handle) != grid.list.type) continue;
                if (!active || grid.focus_moved > active->focus_moved) active = &grid;
            }
            if (active)
                for (const auto& item : active->native)
                    if (item.content == active->shown[static_cast<std::size_t>(active->focused)]) focused = &item;
        };
        find_focus();
        if (const auto steps = sh.kind_steps.exchange(0), pick = sh.kind_pick.exchange(-1); active && (steps || pick >= 0))
            step_kind(*active, steps, pick);
        if (const auto toggles = sh.toggles.exchange(0); (toggles & 1) && focused && !focused->key.empty()) {
            const auto key = focused->key;
            if (!s.favorites.keys.erase(key)) s.favorites.keys.insert(key);
            s.favorites.save();
            logging::log(Level::info, Channel::customization, "Item favorites: {} {}.", key,
                s.favorites.keys.contains(key) ? "added" : "removed");
        }
        ++s.tick;
        for (auto& grid : s.grids) {
            try {
                sync(s, context, grid, rescan);
                if (rescan || (grid.focused >= 0 && s.tick % 3 == 0)) mark(s, context, grid);
            } catch (const std::exception& error) { report(s, error.what()); }
        }
        unmark_stale_focus(context, s.grids);
        find_focus();
        for (const auto& arguments : pokes) {
            try { logging::log(Level::info, Channel::customization, "Item browser: {}", poke(s, context, arguments)); }
            catch (const std::exception& error) { logging::log(Level::warning, Channel::customization, "Item browser: {}: {}", arguments, error.what()); }
        }
        if (sh.status.exchange(false)) { log_status(s); log_roots(s, context); }
        // The search belongs to one visit: it is cleared when the grids are gone, or have been out of
        // focus for longer than one of the game's own cards (a dialog over the grid) usually stays up.
        // A grid keeps its focus after its screen has gone, so the game's menu has to be up as
        // well, and to have been for a moment: the game's menu flag is also up for the second
        // or so it rebuilds the skater after a bail, when the search must not flash up.
        static ULONGLONG menu_since{};
        if (!in_menu) menu_since = 0;
        else if (!menu_since) menu_since = now;
        if (!menu_since || now - menu_since < 1000) active = nullptr, focused = nullptr;
        if (active) s.last_open = now, active->last_shown = now;
        s.open = active != nullptr;
        if (s.open) quiet_notices(s, context);
        s.active_list = active ? active->list.handle : 0;
        // Which grid the overlay is for, when that changes (the first lines of a session, for the log).
        static std::uint64_t shown_list{};
        static unsigned shown_lines = 60;
        if ((active ? active->list.handle : 0) != shown_list) {
            shown_list = active ? active->list.handle : 0;
            if (active && budget(shown_lines))
                logging::log(Level::info, Channel::customization, "Item browser: on screen: grid of slot {} ({} of {} rows, focus {}, type {}).",
                    static_cast<int>(active->slot), active->shown.size(), active->native.size(), active->focused,
                    active->kind.empty() ? std::string_view("All") : active->kind);
        }
        // A type tab belongs to a grid while it is around: one that has been off screen for as long as
        // a visit lasts is shown whole again, so a grid can never stay on a tab without its tabs showing.
        for (auto& grid : s.grids)
            if (&grid != active && !grid.kind.empty() && now > grid.last_shown + 8000) {
                logging::log(Level::info, Channel::customization, "Item browser: grid of slot {} off screen, its type tab {} back to All.",
                    static_cast<int>(grid.slot), grid.kind);
                grid.kind = {};
            }
        if (!s.open && (s.grids.empty() || now > s.last_open + 8000) &&
            (!s.search.empty() || s.filter != Filter::all ||
             std::any_of(s.grids.begin(), s.grids.end(), [](const Grid& grid) { return !grid.kind.empty(); }))) {
            s.search.clear();
            s.filter = Filter::all;
            for (auto& grid : s.grids) grid.kind = {};
        }
        View next;
        next.open = s.open;
        next.search = s.search;
        next.filter = s.filter;
        if (active) {
            const auto blanks = leading_blanks(active->native);
            next.shown = static_cast<unsigned>(active->shown.size()) - (std::min)(blanks, static_cast<unsigned>(active->shown.size()));
            next.total = tile_count(active->native); // fit18: one tile per accessory model
            for (const auto& item : active->native) next.favorites += s.favorites.keys.contains(item.key);
            // Type tabs only where there is more than one kind to choose from.
            if (active->kinds.size() >= 2) {
                KindCount all{"All"};
                for (const auto& kind : active->kinds) { all.total += kind.total; all.shown += kind.shown; }
                next.tabs.push_back(all);
                for (std::size_t i = 0; i < active->kinds.size(); ++i) {
                    next.tabs.push_back(active->kinds[i]);
                    if (active->kinds[i].name == active->kind) next.tab = static_cast<unsigned>(i + 1);
                }
            }
        }
        if (focused) {
            next.focused_title = focused->title;
            next.focused_key = focused->key;
            next.focused_favorite = s.favorites.keys.contains(focused->key);
            next.focused_index = active->focused;
            next.rows = static_cast<unsigned>(active->shown.size());
            next.focused_shows = resolve(context, focused->content).key;
        }
        std::lock_guard shared_lock(sh.mutex);
        sh.view = std::move(next);
    } catch (const std::exception& error) {
        try { report(s, error.what()); } catch (...) {}
    } catch (...) {}
}
} // namespace dingosdk::item_browser
