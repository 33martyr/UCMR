// importer/overlay.cpp: u2import --overlay, the /app0 overlay the launcher lays over Uncharted 2: the main menu with a
// Multiplayer button. Made on the player's PC from their own PS4 game; nothing game-derived ships with the launcher.
//
//   u2data/build/main/dc1/ps4/menu.bin
//
// The Collection's menu script still holds the multiplayer pages (PAGE_MULTIPLAYER and the rest) and the
// MENU_MULTIPLAYER text, but no button leads there. The three main menus get one, each through a new control list
// appended after the data (the page's list pointer moves to it; the old list stays, unused):
//   The Multiplayer control: a copy of the page's BUTTON_MULTIMEDALS record (go-to-page PAGE_MULTIMEDALS with a
//     visibility condition) made a code button: id BUTTON_MULTIPLAYER, label MENU_MULTIPLAYER (the two name strings
//     are appended too), action 0, always visible, no target page. Selecting it calls the game's registered
//     BUTTON_MULTIPLAYER callback, which the runtime restores (gow3_pc/src/u2mp/frontend.cpp).
//   PAGE_NEWPROFILE (no save yet): New Game, Load Game, Multiplayer, Options, Multiplayer Medals; PAGE_NEWPROFILE_BETA:
//     Multiplayer, Beta Trailer, Options, Multiplayer Medals. That is the PS3 order (spec parity-n-remaining.md N3);
//     BUTTON_MULTIMEDALS stays exactly as the game ships it (action 20 to PAGE_MULTIMEDALS, its condition).
//   PAGE_PROFILE (with saves): Continue, Chapters (hidden), Multiplayer (with this page's button flags), the spacer (a
//     row break), then Options and Bonuses in the left column and New Game and Load Game in the right one, as in the
//     original game (no Multiplayer Medals here, as on PS3).
//
// menu.bin is Naughty Dog DC data ('00CD', version 1): u32 data end at +8; 64-bit pointers are offsets into the file;
// after the data a u32 byte count and a bitmap with one bit per 8-byte slot (least significant bit first), set where
// the slot holds a pointer. A name's id is the CRC-32 of the name (MSB first, polynomial 0x04C11DB7, no initial value,
// no final xor). Page record: +0 name, +8 id, +0x38 pointer to {u64 count, u64 base} of 0x70-byte control records:
// +0 name, +8 id, +0x10 kind (2 button, 1 spacer), +0x14 flags, +0x20 label, +0x28 label id, +0x30 action (20 go to
// page), +0x34 visibility condition (0 always), +0x48 target page, +0x50 its id, +0x58 column (0xffffffff: spacer).
// Every value is checked before anything changes; the source is only read. Rebuilt only when the source changed
// (state.json).
#include "u2import.hpp"

namespace u2imp {
namespace {

const char *MENU = "u2data/build/main/dc1/ps4/menu.bin";
// bump when the overlay changes for the same sources (5: Multiplayer is a code button; 6: Multiplayer Medals kept)
const int VERSION = 6;
const size_t RECORD = 0x70;

uint32_t name_id(const std::string &name) {
    uint32_t crc = 0;
    for (unsigned char b : name) {
        crc ^= uint32_t(b) << 24;
        for (int k = 0; k < 8; k++) crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
    }
    return crc;
}
uint32_t u32(const Bytes &d, size_t o) {
    if (o + 4 > d.size()) throw std::runtime_error("struct.error: menu.bin");
    return uint32_t(d[o]) | uint32_t(d[o + 1]) << 8 | uint32_t(d[o + 2]) << 16 | uint32_t(d[o + 3]) << 24;
}
uint64_t u64(const Bytes &d, size_t o) { return uint64_t(u32(d, o)) | uint64_t(u32(d, o + 4)) << 32; }
void p32(Bytes &d, size_t o, uint32_t v) {
    for (int k = 0; k < 4; k++) d.at(o + size_t(k)) = uint8_t(v >> (8 * k));
}
void p64(Bytes &d, size_t o, uint64_t v) {
    p32(d, o, uint32_t(v));
    p32(d, o + 4, uint32_t(v >> 32));
}
// chr(b).isalnum() for a byte read as latin-1
bool py_alnum(uint8_t b) {
    if (isalnum(b)) return true;
    return b == 0xaa || b == 0xb2 || b == 0xb3 || b == 0xb5 || b == 0xb9 || b == 0xba || (b >= 0xbc && b <= 0xbe) || (b >= 0xc0 && b != 0xd7 && b != 0xf7);
}
std::optional<std::string> cstring(const Bytes &d, uint64_t off) {   // the NUL-terminated ASCII name at off
    if (!(0 < off && off < d.size())) return std::nullopt;
    const size_t lim = std::min<size_t>(off + 96, d.size());
    size_t e = off;
    while (e < lim && d[e]) e++;
    if (e >= lim || e <= off) return std::nullopt;
    std::string s(d.begin() + std::ptrdiff_t(off), d.begin() + std::ptrdiff_t(e));
    for (unsigned char c : s)
        if (c > 0x7f) return std::nullopt;
    return s;
}
// every whole copy of name (NUL-terminated, not the end of a longer name)
std::set<uint64_t> strings(const Bytes &d, const std::string &name) {
    std::set<uint64_t> found;
    std::string needle = name;
    needle.push_back('\0');
    for (auto it = std::search(d.begin(), d.end(), needle.begin(), needle.end()); it != d.end(); it = std::search(it + 1, d.end(), needle.begin(), needle.end())) {
        const size_t at = size_t(it - d.begin());
        const uint8_t before = at ? d[at - 1] : 0;
        if (!(py_alnum(before) || before == '_')) found.insert(at);
    }
    return found;
}
// 8-byte aligned pointers to a copy of name: the records that start with that name
std::vector<size_t> records_named(const Bytes &d, const std::string &name) {
    const std::set<uint64_t> targets = strings(d, name);
    std::vector<size_t> v;
    for (size_t at = 0; at + 0x58 < d.size(); at += 8)
        if (targets.count(u64(d, at))) v.push_back(at);
    return v;
}
std::pair<size_t, size_t> pointer_bitmap(const Bytes &d) {
    if (d.size() < 12 || std::string(d.begin(), d.begin() + 4) != "00CD") throw std::runtime_error("ValueError: menu.bin is not DC data");
    const uint32_t end = u32(d, 8), size = u32(d, end);
    if (size_t(end) + 4 + size != d.size() || uint64_t(size) * 8 < end / 8) throw std::runtime_error("ValueError: menu.bin's pointer bitmap is not where expected");
    return {size_t(end) + 4, size};
}
bool is_pointer(const Bytes &d, size_t bitmap, size_t off) { return d.at(bitmap + off / 8 / 8) >> (off / 8 % 8) & 1; }

std::pair<uint64_t, uint64_t> page_controls(const Bytes &d, const std::string &page) {
    std::vector<std::pair<uint64_t, uint64_t>> found;
    for (size_t r : records_named(d, page)) {
        if (u32(d, r + 8) != name_id(page)) continue;
        const uint64_t a = u64(d, r + 0x38);
        if (0 < a && a + 16 < d.size()) found.push_back({u64(d, a), u64(d, a + 8)});
    }
    if (found.size() != 1) throw std::runtime_error(fmt("ValueError: expected one %s page, found %zu", page.c_str(), found.size()));
    return found[0];
}

// A control record to write into a new list, with the offsets (in the record) of its pointer fields.
struct Rec {
    Bytes b;
    std::vector<size_t> fields;
};
// Record `index` of the list at `base` as it is (its pointer fields from the bitmap), `col`: a new column.
Rec old_record(const Bytes &d, size_t bitmap, uint64_t base, size_t index, std::optional<uint32_t> col = std::nullopt) {
    const size_t at = size_t(base) + index * RECORD;
    Rec r{Bytes(d.begin() + std::ptrdiff_t(at), d.begin() + std::ptrdiff_t(at + RECORD)), {}};
    if (col) p32(r.b, 0x58, *col);
    for (size_t f = 0; f < RECORD; f += 8)
        if (is_pointer(d, bitmap, at + f)) r.fields.push_back(f);
    return r;
}

// The one page record named `page`; its control list pointer must be a pointer.
size_t page_record(const Bytes &d, size_t bitmap, const std::string &page) {
    std::vector<size_t> pages;
    for (size_t r : records_named(d, page))
        if (u32(d, r + 8) == name_id(page)) pages.push_back(r);
    if (pages.size() != 1) throw std::runtime_error(fmt("ValueError: expected one %s page, found %zu", page.c_str(), pages.size()));
    if (!is_pointer(d, bitmap, pages[0] + 0x38))
        throw std::runtime_error(fmt("ValueError: %s's control list pointer is not a pointer", page.c_str()));
    return pages[0];
}

// The page's controls are exactly `want` (by name); returns the list base.
uint64_t expect_controls(const Bytes &d, const std::string &page, const std::vector<std::optional<std::string>> &want) {
    const auto [count, base] = page_controls(d, page);
    if (count != want.size())
        throw std::runtime_error(fmt("ValueError: %s has %llu controls, expected %zu", page.c_str(), (unsigned long long)count, want.size()));
    for (size_t i = 0; i < want.size(); i++)
        if (cstring(d, u64(d, base + i * RECORD)) != want[i]) throw std::runtime_error(fmt("ValueError: unexpected %s controls", page.c_str()));
    return base;
}

// The page's shipped BUTTON_MULTIMEDALS record at `at`: go to PAGE_MULTIMEDALS (action 20) with a visibility condition.
void expect_medals(const Bytes &d, size_t bitmap, size_t at, const std::string &page) {
    if (cstring(d, u64(d, at)) != std::optional<std::string>("BUTTON_MULTIMEDALS") || u32(d, at + 8) != name_id("BUTTON_MULTIMEDALS") ||
        u32(d, at + 0x30) != 20 || cstring(d, u64(d, at + 0x48)) != std::optional<std::string>("PAGE_MULTIMEDALS") ||
        u32(d, at + 0x50) != name_id("PAGE_MULTIMEDALS") || !is_pointer(d, bitmap, at + 0x48))
        throw std::runtime_error(fmt("ValueError: %s's medals button is not as expected (not the Uncharted 2 1.02 menu?)", page.c_str()));
}

// The Multiplayer control: a copy of the medals record `medals` made a code button (BUTTON_MULTIPLAYER, label
// MENU_MULTIPLAYER, action 0, always visible, no target page: a null target is not relocated), its name strings at
// `name` / `label`.
Rec multiplayer_record(const Bytes &d, size_t medals, size_t name, size_t label) {
    Rec r{Bytes(d.begin() + std::ptrdiff_t(medals), d.begin() + std::ptrdiff_t(medals + RECORD)), {0x00, 0x20}};
    p64(r.b, 0x00, name);
    p64(r.b, 0x08, name_id("BUTTON_MULTIPLAYER"));
    p64(r.b, 0x20, label);
    p64(r.b, 0x28, name_id("MENU_MULTIPLAYER"));
    p32(r.b, 0x30, 0);
    p32(r.b, 0x34, 0);
    p64(r.b, 0x48, 0);
    p64(r.b, 0x50, 0);
    return r;
}

// PAGE_PROFILE's new controls (`mp`: the Multiplayer control of PAGE_NEWPROFILE).
std::vector<Rec> profile_records(const Bytes &d, size_t bitmap, Rec mp) {
    const uint64_t base = expect_controls(d, "PAGE_PROFILE", {"BUTTON_CONTINUE", "BUTTON_PROFILE_CHAPTERS", std::nullopt, "BUTTON_NEWGAME",
                                                              "BUTTON_LOADGAME", "BUTTON_OPTIONS", "BUTTON_BONUSES"});
    const size_t spacer = size_t(base) + 2 * RECORD;
    bool sp_ptr = false;
    for (size_t f = 0; f < RECORD; f += 8) sp_ptr = sp_ptr || is_pointer(d, bitmap, spacer + f);
    if (u32(d, spacer + 0x10) != 1 || u32(d, spacer + 0x14) != 0 || u32(d, spacer + 0x58) != 0xffffffffu || sp_ptr)
        throw std::runtime_error("ValueError: PAGE_PROFILE's spacer is not as expected");
    for (const auto &[index, column] : std::vector<std::pair<size_t, uint32_t>>{{3, 0}, {4, 0}, {5, 1}, {6, 1}})
        if (u32(d, base + index * RECORD + 0x58) != column) throw std::runtime_error("ValueError: PAGE_PROFILE's columns are not as expected");
    p32(mp.b, 0x14, u32(d, base + 3 * RECORD + 0x14));
    p32(mp.b, 0x58, 0);
    auto old = [&](size_t index, std::optional<uint32_t> col = std::nullopt) { return old_record(d, bitmap, base, index, col); };
    return {old(0), old(1), mp, old(2), old(5, 0), old(6, 0), old(3, 1), old(4, 1)};
}

size_t patch_menu(Bytes &d) {
    if (strings(d, "PAGE_MULTIPLAYER").empty()) throw std::runtime_error("ValueError: menu.bin has no PAGE_MULTIPLAYER (not Uncharted 2?)");
    const auto [bitmap, bsz] = pointer_bitmap(d);
    (void)bsz;
    const uint32_t end = u32(d, 8);
    const size_t profile = page_record(d, bitmap, "PAGE_PROFILE"), fresh = page_record(d, bitmap, "PAGE_NEWPROFILE"),
                 beta = page_record(d, bitmap, "PAGE_NEWPROFILE_BETA");
    const uint64_t fresh_base = expect_controls(d, "PAGE_NEWPROFILE", {"BUTTON_NEWGAME", "BUTTON_LOADGAME", "BUTTON_OPTIONS", "BUTTON_MULTIMEDALS"});
    const uint64_t beta_base = expect_controls(d, "PAGE_NEWPROFILE_BETA", {"BUTTON_BETA_TRAILER", "BUTTON_OPTIONS", "BUTTON_MULTIMEDALS"});
    expect_medals(d, bitmap, size_t(fresh_base) + 3 * RECORD, "PAGE_NEWPROFILE");
    expect_medals(d, bitmap, size_t(beta_base) + 2 * RECORD, "PAGE_NEWPROFILE_BETA");

    // After the old data, 16-byte aligned: the two name strings, then each new list ({u64 count, u64 base}, records).
    const size_t start = (size_t(end) + 15) & ~size_t(15), name = start, label = start + 24, lists = start + 48;
    Bytes blob(lists - end, 0);
    const std::string name_s = "BUTTON_MULTIPLAYER", label_s = "MENU_MULTIPLAYER";
    std::copy(name_s.begin(), name_s.end(), blob.begin() + std::ptrdiff_t(name - end));
    std::copy(label_s.begin(), label_s.end(), blob.begin() + std::ptrdiff_t(label - end));
    const Rec fresh_mp = multiplayer_record(d, size_t(fresh_base) + 3 * RECORD, name, label);
    const Rec beta_mp = multiplayer_record(d, size_t(beta_base) + 2 * RECORD, name, label);
    auto fresh_old = [&](size_t index) { return old_record(d, bitmap, fresh_base, index); };
    auto beta_old = [&](size_t index) { return old_record(d, bitmap, beta_base, index); };
    const std::vector<std::pair<size_t, std::vector<Rec>>> pages = {
        {profile, profile_records(d, bitmap, fresh_mp)},
        {fresh, {fresh_old(0), fresh_old(1), fresh_mp, fresh_old(2), fresh_old(3)}},
        {beta, {beta_mp, beta_old(0), beta_old(1), beta_old(2)}},
    };
    std::vector<size_t> pointers, headers;
    for (const auto &[page, records] : pages) {
        const size_t header = end + blob.size(), first = header + 16;
        headers.push_back(header);
        blob.resize(blob.size() + 16, 0);
        p64(blob, header - end, records.size());
        p64(blob, header - end + 8, first);
        pointers.push_back(header + 8);
        for (size_t i = 0; i < records.size(); i++) {
            blob.insert(blob.end(), records[i].b.begin(), records[i].b.end());
            for (size_t f : records[i].fields) pointers.push_back(first + i * RECORD + f);
        }
    }
    const size_t new_end = end + blob.size(), slots = (new_end + 7) / 8;
    Bytes bits((slots + 7) / 8, 0);
    const Bytes old_bits(d.begin() + std::ptrdiff_t(bitmap), d.end());
    if (old_bits.size() > bits.size()) bits.resize(old_bits.size(), 0);   // (Python's slice assignment would grow it)
    std::copy(old_bits.begin(), old_bits.end(), bits.begin());
    for (size_t off : pointers) bits.at(off / 64) |= uint8_t(1 << (off / 8 % 8));
    Bytes rebuilt(d.begin(), d.begin() + end);
    rebuilt.insert(rebuilt.end(), blob.begin(), blob.end());
    p32(rebuilt, 8, uint32_t(new_end));
    for (size_t i = 0; i < pages.size(); i++) p64(rebuilt, pages[i].first + 0x38, headers[i]);
    for (int k = 0; k < 4; k++) rebuilt.push_back(uint8_t(bits.size() >> (8 * k)));
    rebuilt.insert(rebuilt.end(), bits.begin(), bits.end());
    d = rebuilt;
    return pages.size();
}

}  // namespace

// the overlay folder `out` (made or kept); throws with the reason
fs::path build_overlay(const fs::path &game, const fs::path &out, const std::optional<fs::path> &update) {
    std::optional<fs::path> menu;
    std::error_code ec;
    for (const auto &root : {update, std::optional<fs::path>(game)}) {
        if (!root || menu) continue;
        for (const fs::path &base : {*root, *root / "uroot"})
            if (fs::is_regular_file(base / P(MENU), ec)) {
                menu = base / P(MENU);
                break;
            }
    }
    if (!menu) throw std::runtime_error(std::string("FileNotFoundError: ") + MENU + " not found in the game");
    const auto t = fs::last_write_time(*menu, ec);
    const long long mtime = (long long)std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
    const std::string want = dumps(obj({{"version", num(VERSION)}, {"menu", [&] {
                                             Value a = Value::object();
                                             a.kind = Value::Array;
                                             a.items = {str(S(*menu)), num((long long)fs::file_size(*menu)), num(mtime)};
                                             return a;
                                         }()}}));
    const fs::path state = out / "state.json";
    if (fs::is_regular_file(state, ec) && read_text(state) == want && fs::is_regular_file(out / P(MENU), ec)) return out;
    Bytes data = read_bytes(*menu);
    const size_t n = patch_menu(data);
    const fs::path target = out / P(MENU), tmp = S(target) + ".tmp";
    fs::create_directories(target.parent_path(), ec);
    write_bytes(tmp, data);
    fs::rename(tmp, target, ec);
    if (ec) throw std::runtime_error("cannot write " + S(target) + ": " + ec.message());
    write_text(state, want);
    log(fmt("u2mp: Multiplayer button in the main menus, Multiplayer Medals kept (%zu pages) -> %s", n, S(target).c_str()));
    return out;
}

}  // namespace u2imp
