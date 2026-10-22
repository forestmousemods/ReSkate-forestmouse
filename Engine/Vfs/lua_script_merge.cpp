#include "lua_script_merge.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace dingosdk::mods::detail {
namespace {

template<class T> bool read_number(std::span<const std::byte> bytes, std::size_t at, T& value) {
    if (at + sizeof(T) > bytes.size()) return false;
    std::memcpy(&value, bytes.data() + at, sizeof(T));
    return true;
}
template<class T> void put_number(std::vector<std::byte>& out, T value) {
    const auto at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &value, sizeof(T));
}

// Lines with their endings kept ("...\r\n"), so the merge gives back the text exactly.
std::vector<std::string_view> lines_of(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t at = 0;
    while (at < text.size()) {
        const auto end = text.find('\n', at);
        const auto next = end == std::string_view::npos ? text.size() : end + 1;
        lines.push_back(text.substr(at, next - at));
        at = next;
    }
    return lines;
}

// For each line of `base`, the line of `other` it is kept as (or -1): a longest common subsequence, the common
// beginning and end taken off first (two versions of a script differ in a few places). Empty when too big to match.
std::optional<std::vector<std::ptrdiff_t>> match(const std::vector<std::string_view>& base,
                                                 const std::vector<std::string_view>& other) {
    const std::size_t n = base.size(), m = other.size();
    std::vector<std::ptrdiff_t> result(n, -1);
    std::size_t head = 0;
    while (head < n && head < m && base[head] == other[head]) result[head] = static_cast<std::ptrdiff_t>(head), ++head;
    std::size_t tail = 0;
    while (tail < n - head && tail < m - head && base[n - 1 - tail] == other[m - 1 - tail]) {
        result[n - 1 - tail] = static_cast<std::ptrdiff_t>(m - 1 - tail);
        ++tail;
    }
    const std::size_t rows = n - head - tail, cols = m - head - tail;
    if (!rows || !cols) return result;
    if (rows * cols > 16'000'000) return std::nullopt;
    // table[i][j]: the LCS of base[head + i ..] and other[head + j ..]
    std::vector<std::uint32_t> table((rows + 1) * (cols + 1), 0);
    const auto cell = [&](std::size_t i, std::size_t j) -> std::uint32_t& { return table[i * (cols + 1) + j]; };
    for (std::size_t i = rows; i-- > 0;)
        for (std::size_t j = cols; j-- > 0;)
            cell(i, j) = base[head + i] == other[head + j] ? cell(i + 1, j + 1) + 1 : std::max(cell(i + 1, j), cell(i, j + 1));
    for (std::size_t i = 0, j = 0; i < rows && j < cols;) {
        if (base[head + i] == other[head + j]) result[head + i] = static_cast<std::ptrdiff_t>(head + j), ++i, ++j;
        else if (cell(i + 1, j) >= cell(i, j + 1)) ++i;
        else ++j;
    }
    return result;
}

bool same(std::span<const std::string_view> a, std::span<const std::string_view> b) {
    return std::ranges::equal(a, b);
}
void append(std::string& out, std::span<const std::string_view> lines) {
    for (const auto line : lines) out += line;
}

} // namespace

std::optional<LuaScript> read_lua_script(std::span<const std::byte> resource) {
    std::uint64_t nameAt{}, sourceAt{}, tableAt{};
    std::uint32_t zero{}, length{};
    if (!read_number(resource, 0, nameAt) || !read_number(resource, 8, sourceAt) || !read_number(resource, 16, tableAt) ||
        !read_number(resource, 24, zero) || !read_number(resource, 28, length))
        return std::nullopt;
    if (nameAt != 0x20 || zero != 0 || sourceAt <= nameAt || sourceAt + length + 1 > resource.size() ||
        tableAt + 12 != resource.size() || tableAt < sourceAt + length + 1 || tableAt % 8)
        return std::nullopt;
    std::uint32_t table[3]{};
    for (int k = 0; k < 3; ++k)
        if (!read_number(resource, tableAt + 4 * k, table[k])) return std::nullopt;
    if (table[0] != 0 || table[1] != 8 || table[2] != 16) return std::nullopt;
    const auto* bytes = reinterpret_cast<const char*>(resource.data());
    const std::string_view name(bytes + nameAt, sourceAt - nameAt - 1);
    if (bytes[sourceAt - 1] != '\0' || name.find('\0') != std::string_view::npos || bytes[sourceAt + length] != '\0')
        return std::nullopt;
    LuaScript script{std::string(name), std::string(bytes + sourceAt, length)};
    if (write_lua_script(script).size() != resource.size()) return std::nullopt;   // a layout this cannot give back
    return script;
}

std::vector<std::byte> write_lua_script(const LuaScript& script) {
    const std::uint64_t sourceAt = 0x20 + script.name.size() + 1;
    const std::uint64_t end = sourceAt + script.source.size() + 1;
    const std::uint64_t tableAt = (end + 7) & ~std::uint64_t{7};
    std::vector<std::byte> out;
    out.reserve(tableAt + 12);
    put_number(out, std::uint64_t{0x20});
    put_number(out, sourceAt);
    put_number(out, tableAt);
    put_number(out, std::uint32_t{0});
    put_number(out, static_cast<std::uint32_t>(script.source.size()));
    for (const char c : script.name) out.push_back(static_cast<std::byte>(c));
    out.push_back(std::byte{0});
    for (const char c : script.source) out.push_back(static_cast<std::byte>(c));
    out.resize(tableAt, std::byte{0});
    put_number(out, std::uint32_t{0});
    put_number(out, std::uint32_t{8});
    put_number(out, std::uint32_t{16});
    return out;
}

std::array<std::byte, 16> lua_script_meta(std::span<const std::byte> resource) {
    std::array<std::byte, 16> meta{};
    std::uint64_t tableAt{};
    (void)read_number(resource, 16, tableAt);
    const auto at = static_cast<std::uint32_t>(tableAt);
    const std::uint32_t size = 12;
    std::memcpy(meta.data(), &at, 4);
    std::memcpy(meta.data() + 4, &size, 4);
    return meta;
}

// A version whose lines end otherwise than the game's (Kori's Skaterize keeps its copy of the skater loader with LF
// endings, the game's has CRLF): every line would differ. Merged with the game's endings.
std::string with_line_ends_of(const std::string& game, const std::string& text) {
    const bool crlf = game.find("\r\n") != std::string::npos;
    const bool textCrlf = text.find("\r\n") != std::string::npos;
    const bool textLf = text.find('\n') != std::string::npos;
    if (crlf == textCrlf || !textLf) return text;
    std::string out;
    out.reserve(text.size() + text.size() / 16);
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue;   // CRLF -> LF first
        if (text[i] == '\n' && crlf) out += '\r';
        out += text[i];
    }
    return out;
}

std::optional<std::string> merge_lua_source(const std::string& game, const std::string& higherText, const std::string& lowerText) {
    const auto higher = with_line_ends_of(game, higherText), lower = with_line_ends_of(game, lowerText);
    const auto base = lines_of(game), a = lines_of(higher), b = lines_of(lower);
    const auto ma = match(base, a), mb = match(base, b);
    if (!ma || !mb) return std::nullopt;
    std::string out;
    out.reserve(std::max(higher.size(), lower.size()) + 4096);
    std::size_t fromBase = 0, fromA = 0, fromB = 0;
    for (std::size_t i = 0; i <= base.size(); ++i) {
        // A line both versions kept is where the versions line up again (the end counts as one).
        if (i < base.size() && ((*ma)[i] < 0 || (*mb)[i] < 0)) continue;
        const auto toA = i < base.size() ? static_cast<std::size_t>((*ma)[i]) : a.size();
        const auto toB = i < base.size() ? static_cast<std::size_t>((*mb)[i]) : b.size();
        if (toA < fromA || toB < fromB) return std::nullopt;
        const std::span<const std::string_view> was(base.data() + fromBase, i - fromBase);
        const std::span<const std::string_view> inA(a.data() + fromA, toA - fromA), inB(b.data() + fromB, toB - fromB);
        if (same(inA, inB) || same(inB, was)) append(out, inA);
        else if (same(inA, was)) append(out, inB);
        else return std::nullopt;   // both changed these lines, differently
        if (i < base.size()) out += base[i];
        fromBase = i + 1, fromA = toA + 1, fromB = toB + 1;
    }
    return out;
}

std::optional<std::vector<std::byte>> merge_lua_script(std::span<const std::byte> game, std::span<const std::byte> higher,
                                                       std::span<const std::byte> lower) {
    const auto g = read_lua_script(game), h = read_lua_script(higher), l = read_lua_script(lower);
    if (!g || !h || !l || h->name != g->name || l->name != g->name) return std::nullopt;
    auto source = merge_lua_source(g->source, h->source, l->source);
    if (!source) return std::nullopt;
    return write_lua_script({g->name, std::move(*source)});
}

} // namespace dingosdk::mods::detail
