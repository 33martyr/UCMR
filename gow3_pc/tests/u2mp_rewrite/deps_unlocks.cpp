// u2mp rewrite test "unlocks": what unlocks.cpp needs from other modules, compiled for both builds (run.sh).
// Every stand-in forwards to the test (test_unlocks.cpp, namespace ul), which records the call (hx::rec) and
// answers from the scenario's state. runtime_file_translate is renamed for the old build (flags_unlocks.sh).
#include <cstddef>
#include <cstdint>
#include <string>

#include "harness.h"
#include "u2mp_internal.h"

namespace ul {
void *netheap_alloc(size_t size, size_t align);
const uint8_t *package_by_id(uint32_t id);
int translate(const char *guest, char *out, size_t size);
}  // namespace ul

namespace u2mp {
void *netheap_alloc(size_t size, size_t align) { return ul::netheap_alloc(size, align); }
const uint8_t *package_by_id(uint32_t id) { return ul::package_by_id(id); }
#if !defined(u2mp)   // new build only: unlocks_apply_slot_skins (6b34909, issue #34) asks session.cpp for a remote
                     // member's skins; the old headers lack it. No scenario has party members: none is known.
namespace session {
bool member_skins(uint32_t net_id, std::string &, std::string &) {
    hx::rec("session::member_skins %u -> 0", net_id);
    return false;
}
}  // namespace session
#endif
}  // namespace u2mp

extern "C" int runtime_file_translate(const char *guest, char *out, size_t size) { return ul::translate(guest, out, size); }
