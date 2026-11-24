// dcconv/menu_pages.hpp: PS3 menu pages converted into the PS4 menu.bin (dc-recipes, see menu_pages.cpp)
#pragma once
#include "dc.hpp"

namespace u3::menu_pages {

// the page table of menu.bin: DC symbol 0x74ffceeb = { u32 count, pointer to the page records }
constexpr uint32_t PAGE_TABLE = 0x74ffceeb;
// PAGE_MULTIPLAYER_MOTD (CRC-32 name id): the message-of-the-day page PS3 1.19 shows after connecting; PS4 has none
constexpr uint32_t PAGE_MULTIPLAYER_MOTD = 0xd6f98d11;

// one PS3 record (page 0x4c, element group 0x0c, element 0x48 bytes) converted to its PS4 form (0x80, 0x18, 0x68);
// pointers become 8-byte slots whose targets the caller fills in (returned as PS4 slot offset -> PS3 target).
// Throws Stop on a value whose PS4 form is not known from the pages both games have.
struct Converted {
    Bytes data;
    std::vector<std::pair<uint64_t, uint64_t>> ptrs;   // PS4 slot offset in data, PS3 target offset
};
Converted page_record(const Dc &ps3, uint64_t rec);
Converted group_record(const Dc &ps3, uint64_t grp);
Converted element_record(const Dc &ps3, uint64_t el);

// the PS4 menu.bin `ps4` with PS3 menu.bin `ps3`'s page `id` added (record, its element group, the elements and
// their strings appended, the page table rebuilt with the page at the end); unchanged when `ps4` has the page already
Bytes add_page(const Bytes &ps4, const Bytes &ps3, uint32_t id);

}  // namespace u3::menu_pages
