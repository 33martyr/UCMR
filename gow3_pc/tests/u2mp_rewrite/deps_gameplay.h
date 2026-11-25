// u2mp rewrite tests, gameplay group: the answers of the other modules' stand-ins (deps_gameplay.cpp), set by the
// test (test_gameplay.cpp). One copy for both namespaces; scenario() resets it before each side.
#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>

struct GpDeps {
    bool text_in_rect = true;                      // loading.cpp menu_text_in_rect: the text was drawn
    bool menu_up = false;                          // loading.cpp menu_up: a menu is up (not recorded)
    uint32_t local_id = 1;                         // session::local_id: this PC's net id (the host's)
    std::map<uint32_t, std::string> members;       // session::member_name: the party's members by net id
    std::set<uint32_t> muted;                      // voice::muted / toggle_mute: the muted net ids
};
inline GpDeps gp_deps;
