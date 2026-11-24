// dcconv/menu_pages.cpp: a PS3 menu page converted into the PS4 menu.bin, for the pages PS3 1.19 has and the PS4
// menu.bin lacks that the game needs (dc-recipes: PAGE_MULTIPLAYER_MOTD, owner's decision 2026-11-24).
//
// menu.bin's page table (symbol 0x74ffceeb) lists page records; a record points at one element group (element count,
// the element records, a type id), the elements and records point at their name strings. The PS4 records keep the
// PS3 fields in the same order; a pointer becomes 8 bytes and the name id after it is 8 bytes too (u32 + padding).
// The layouts below follow that rule and were checked against the 122 pages both games have (every field that is set
// in those pages lands where PS4 has it). Values stay as PS3 has them (PS4 moved some pages' coordinates; a page
// converted here keeps PS3's). A field whose PS4 form those pages do not settle (a flag bit no page agrees on, a list
// or slider element's flags, which PS4 packs differently) stops the step instead of guessing.
#include "menu_pages.hpp"

#include <array>

namespace u3::menu_pages {
namespace {

enum Kind { W, P, H, PAGE_FLAGS, ELEMENT_FLAGS };   // raw word, pointer, 16-bit halves swapped, flags
struct Field {
    uint32_t ps3, ps4;
    Kind kind;
};
constexpr uint64_t PAGE3 = 0x4c, PAGE4 = 0x80, GROUP3 = 0x0c, GROUP4 = 0x18, ELEMENT3 = 0x48, ELEMENT4 = 0x68;

// page record: (name, id), three (string, id) pairs, a word, flags, element group, four layout words, then two
// (button name, id) pairs: the button the page starts on and the one that leaves it (only the MOTD page sets the
// latter; it takes the last 16 bytes of the 0x80-byte PS4 record, the place the rule above gives it)
constexpr std::array<Field, 19> PAGE_FIELDS{{{0x00, 0x00, P},  {0x04, 0x08, W},  {0x08, 0x10, P},          {0x0c, 0x18, W},
                                              {0x10, 0x20, P},  {0x14, 0x28, W},  {0x18, 0x30, P},          {0x1c, 0x38, W},
                                              {0x20, 0x40, W},  {0x24, 0x44, PAGE_FLAGS}, {0x28, 0x48, P},  {0x2c, 0x50, W},
                                              {0x30, 0x54, W},  {0x34, 0x58, W},  {0x38, 0x5c, W},          {0x3c, 0x60, P},
                                              {0x40, 0x68, W},  {0x44, 0x70, P},  {0x48, 0x78, W}}};
// element group: element count, the element records, their type id
constexpr std::array<Field, 3> GROUP_FIELDS{{{0x00, 0x00, W}, {0x04, 0x08, P}, {0x08, 0x14, W}}};
// element: name, id, type, flags, five words, text (string, id), layout words, a second string and its id, and four
// words whose 16-bit halves PS4 stores the other way round
constexpr std::array<Field, 18> ELEMENT_FIELDS{{{0x00, 0x00, P}, {0x04, 0x08, W}, {0x08, 0x10, W}, {0x0c, 0x14, ELEMENT_FLAGS},
                                                 {0x10, 0x18, W}, {0x14, 0x20, P}, {0x18, 0x28, W}, {0x1c, 0x30, W},
                                                 {0x20, 0x34, W}, {0x24, 0x38, W}, {0x28, 0x3c, W}, {0x2c, 0x40, W},
                                                 {0x30, 0x48, P}, {0x34, 0x50, W}, {0x38, 0x58, H}, {0x3c, 0x5c, H},
                                                 {0x40, 0x60, H}, {0x44, 0x64, H}}};
// page flag bits whose PS4 position every page both games have agrees on (PS3 bit, PS4 bit)
constexpr std::array<std::pair<int, int>, 9> PAGE_FLAG_BITS{{{0, 0}, {1, 1}, {9, 9}, {10, 10}, {12, 12}, {13, 13}, {20, 18}, {21, 19}, {23, 21}}};
// element types whose flags word PS4 packs differently (lists 7, sliders 8); every other type keeps it as it is
constexpr uint32_t LIST = 7, SLIDER = 8;

template <size_t N>
Converted convert(const Dc &ps3, uint64_t o, uint64_t size3, uint64_t size4, const std::array<Field, N> &fields, const char *what) {
    if (o + size3 > ps3.d.size()) throw Stop(fmt("menu.bin (PS3): %s at %llx is past the end", what, (unsigned long long)o));
    Converted c;
    c.data.assign(size4, 0);
    for (const Field &f : fields) {
        const uint64_t a = o + f.ps3;
        const uint32_t v = ps3.u32(a);
        if (f.kind == P) {
            if (ps3.isptr(a)) c.ptrs.push_back({f.ps4, ps3.ptr(a)});
            else if (v) throw Stop(fmt("menu.bin (PS3): %s at %llx: +%x is neither a pointer nor 0", what, (unsigned long long)o, f.ps3));
            continue;
        }
        if (ps3.isptr(a)) throw Stop(fmt("menu.bin (PS3): %s at %llx: unexpected pointer at +%x", what, (unsigned long long)o, f.ps3));
        uint32_t w = v;
        switch (f.kind) {
        case H: w = v << 16 | v >> 16; break;
        case PAGE_FLAGS: {
            w = 0;
            uint32_t left = v;
            for (auto [b3, b4] : PAGE_FLAG_BITS)
                if (v >> b3 & 1) w |= 1u << b4, left &= ~(1u << b3);
            if (left) throw Stop(fmt("menu.bin (PS3): page at %llx: flag bits %08x have no known PS4 place", (unsigned long long)o, left));
            break;
        }
        case ELEMENT_FLAGS: {
            const uint32_t type = ps3.u32(o + 0x08);
            if (v && (type == LIST || type == SLIDER))
                throw Stop(fmt("menu.bin (PS3): element at %llx: type %u flags %08x, PS4 packs them differently", (unsigned long long)o, type, v));
            break;
        }
        default: break;
        }
        wr32le(&c.data[f.ps4], w);
    }
    return c;
}

const Sym &page_table(const Dc &c) {
    const Sym *t = nullptr;
    for (const Sym &s : c.syms)
        if (s.sid == PAGE_TABLE) {
            if (t) throw Stop(c.name + ": page table symbol found twice");
            t = &s;
        }
    if (!t) throw Stop(c.name + ": no page table symbol");
    return *t;
}

}  // namespace

Converted page_record(const Dc &ps3, uint64_t rec) { return convert(ps3, rec, PAGE3, PAGE4, PAGE_FIELDS, "page"); }
Converted group_record(const Dc &ps3, uint64_t grp) { return convert(ps3, grp, GROUP3, GROUP4, GROUP_FIELDS, "element group"); }
Converted element_record(const Dc &ps3, uint64_t el) { return convert(ps3, el, ELEMENT3, ELEMENT4, ELEMENT_FIELDS, "element"); }

Bytes add_page(const Bytes &ps4, const Bytes &ps3, uint32_t id) {
    const Dc p3(ps3, "menu.bin (PS3)"), p4(ps4, "menu.bin");
    if (!p3.be || p4.be) throw Stop("menu pages: expected a PS3 and a PS4 menu.bin");
    const Sym &t3 = page_table(p3), &t4 = page_table(p4);
    const uint32_t n3 = p3.u32(t3.off), n4 = p4.u32(t4.off);
    const uint64_t arr3 = p3.ptr(t3.off + 4), arr4 = p4.ptr(t4.off + 8);
    for (uint32_t k = 0; k < n4; k++)
        if (p4.u32(arr4 + k * PAGE4 + 8) == id) return ps4;   // already there
    uint64_t rec3 = 0;
    for (uint32_t k = 0; k < n3 && !rec3; k++)
        if (p3.u32(arr3 + k * PAGE3 + 4) == id) rec3 = arr3 + k * PAGE3;
    if (!rec3) throw Stop(fmt("menu.bin (PS3): no page %08x", id));

    // the PS4 module up to its relocation bitmap; new objects are appended, the original bytes stay (append-only, as
    // the other menu.bin edits do), except the page table's count and pointer
    if (p4.reloc + 4 + p4.u32(p4.reloc) != ps4.size()) throw Stop("menu.bin: unexpected data after the relocation bitmap");
    Bytes d(ps4.begin(), ps4.begin() + std::ptrdiff_t(p4.reloc));
    std::set<uint64_t> ptrs = p4.ptrs;
    std::vector<std::pair<uint64_t, uint64_t>> fix;   // PS4 slot -> PS3 string to append
    auto align = [&](size_t a) { while (d.size() % a) d.push_back(0); };
    auto place = [&](const Converted &c, uint64_t at) {
        std::copy(c.data.begin(), c.data.end(), d.begin() + std::ptrdiff_t(at));
        for (auto [slot, target] : c.ptrs) fix.push_back({at + slot, target});
    };

    // the element group and its elements
    const Converted page = page_record(p3, rec3);
    uint64_t grp3 = 0;
    for (auto [slot, target] : page.ptrs)
        if (slot == 0x48) grp3 = target;
    if (!grp3) throw Stop(fmt("menu.bin (PS3): page %08x has no element group", id));
    const Converted group = group_record(p3, grp3);
    const uint32_t count = p3.u32(grp3);
    uint64_t els3 = 0;
    for (auto [slot, target] : group.ptrs) els3 = target;
    if (count && !els3) throw Stop(fmt("menu.bin (PS3): page %08x: %u elements without a pointer", id, count));
    align(16);
    const uint64_t els4 = d.size();
    d.resize(d.size() + count * ELEMENT4, 0);
    for (uint32_t k = 0; k < count; k++) place(element_record(p3, els3 + k * ELEMENT3), els4 + k * ELEMENT4);
    align(16);
    const uint64_t grp4 = d.size();
    d.resize(d.size() + GROUP4, 0);
    place(group, grp4);

    // the new page table: the PS4 pages, then this page
    align(16);
    const uint64_t tab = d.size();
    d.resize(d.size() + (uint64_t(n4) + 1) * PAGE4, 0);
    std::copy(ps4.begin() + std::ptrdiff_t(arr4), ps4.begin() + std::ptrdiff_t(arr4 + uint64_t(n4) * PAGE4), d.begin() + std::ptrdiff_t(tab));
    for (uint64_t k = 0; k < uint64_t(n4) * PAGE4; k += 8)
        if (p4.ptrs.count(arr4 + k)) ptrs.insert(tab + k);
    place(page, tab + uint64_t(n4) * PAGE4);

    // pointer targets: the group's elements, the page's group, every other pointer is a name string (appended once)
    std::map<uint64_t, uint64_t> strings;
    for (auto [slot, target] : fix) {
        uint64_t v;
        if (target == els3) v = els4;
        else if (target == grp3) v = grp4;
        else if (auto it = strings.find(target); it != strings.end()) v = it->second;
        else {
            const std::optional<std::string> s = p3.cstr(target);
            if (!s || s->empty()) throw Stop(fmt("menu.bin (PS3): page %08x: pointer to %llx is not a string", id, (unsigned long long)target));
            v = strings[target] = d.size();
            d.insert(d.end(), s->begin(), s->end());
            d.push_back(0);
        }
        wr64le(&d[slot], v);
        ptrs.insert(slot);
    }
    wr32le(&d[t4.off], n4 + 1);
    wr64le(&d[t4.off + 8], tab);

    // relocation bitmap (one bit per 8-byte word, LSB first) after the 16-aligned body, as DcEdit::build writes it
    align(16);
    const uint64_t r = d.size(), nb = (r + 63) / 64;
    Bytes bm(nb, 0);
    for (uint64_t p : ptrs) {
        if (p >= r || p % 8) throw Stop(fmt("menu pages: pointer slot %llx", (unsigned long long)p));
        bm[p / 64] |= uint8_t(1u << (p / 8 % 8));
    }
    wr64le(&d[8], r);
    uint8_t cnt[4];
    wr32le(cnt, uint32_t(nb));
    d.insert(d.end(), cnt, cnt + 4);
    d.insert(d.end(), bm.begin(), bm.end());
    return d;
}

}  // namespace u3::menu_pages
