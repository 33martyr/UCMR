// u2mp rewrite tests, gameplay group: what the seven sources need from other modules, compiled for both namespaces.
// package_by_id (session.cpp) is recorded and answered by the test (test_gameplay.cpp: gp_package_by_id); the
// new-only stand-ins are recorded and answered from gp_deps (deps_gameplay.h).
#include "u2mp_internal.h"
#include "harness.h"

#include "deps_gameplay.h"

extern const uint8_t *(*gp_package_by_id)(uint32_t id);

namespace u2mp {

const uint8_t *package_by_id(uint32_t id) {
    hx::rec("package_by_id %08x", id);
    return gp_package_by_id ? gp_package_by_id(id) : nullptr;
}

}  // namespace u2mp

// Only the new sources call these (only the new headers declare them): scoreboard.cpp's hint line, voice column and
// row mute action (parity-m-features.md M1, parity-j-voice-medals.md J4/J5), the NetStatGame rank copy
// (scoreboard.cpp, rosternames.cpp), and the pure overlay rules, which are compiled in so their real behaviour runs.
#if !defined(u2mp)
#include "session_api.h"
#include "voice.h"

#include "overlay_rules.cpp"

namespace u2mp {

// loading.cpp
bool menu_text_in_rect(const float *xform, const char *text, float x, float y, float w, float h) {
    hx::rec("menu_text_in_rect scale=%g alpha=%g [%s] (%g, %g, %g, %g)", double(xform[0]), double(xform[4]),
            text ? text : "(NULL)", double(x), double(y), double(w), double(h));
    return gp_deps.text_in_rect;
}

// loading.cpp: the menu state word (scoreboard.cpp's hint line and page draw); no menu is up in these scenarios
bool menu_up() { return gp_deps.menu_up; }

// session.cpp
void sync_netstat_ranks() { hx::rec("sync_netstat_ranks"); }

namespace session {
uint32_t local_id() {
    hx::rec("session::local_id");
    return gp_deps.local_id;
}
bool member_name(uint32_t net_id, std::string &name) {
    hx::rec("session::member_name %u", net_id);
    auto it = gp_deps.members.find(net_id);
    if (it == gp_deps.members.end()) return false;
    name = it->second;
    return true;
}
}  // namespace session

// voice.cpp
namespace voice {
bool muted(uint32_t net_id) {
    hx::rec("voice::muted %u", net_id);
    return gp_deps.muted.count(net_id) != 0;
}
bool toggle_mute(uint32_t net_id) {
    hx::rec("voice::toggle_mute %u", net_id);
    if (gp_deps.muted.erase(net_id)) return false;
    gp_deps.muted.insert(net_id);
    return true;
}
}  // namespace voice

}  // namespace u2mp
#endif
