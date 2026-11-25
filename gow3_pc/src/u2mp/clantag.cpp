// u2mp: editing the clan tag on the multiplayer profile page.
//
// On PS3 BUTTON_CLANTAG opened the on-screen keyboard and the page polled it every frame. On PC the
// runtime's in-window text input takes its place (Enter or Cross accepts, Esc or Circle cancels):
// remaining.cpp's button callback opens it with edit_clan_tag(), and the page's per-frame event calls
// clan_tag_frame() until it closes. The tag is kept in NetPlayerData, where the game's label query
// shows it and progression.cpp saves and publishes it.
//
// While the game is online (NET_SM > 1), clan_tag_check_frame() checks every new tag before it counts and then
// publishes the career and the member data, as PS3 did with its score service's comment censor (u2mp/docs/spec/
// parity-a-leaderboards.md 2.1). A refused tag, or one whose check failed, becomes "*".
//
// Reimplemented from the behaviour descriptions u2mp/docs/spec/remaining.md and parity-a-leaderboards.md.
#include "online.h"
#include "progression.h"
#include "u2mp_internal.h"

#include <cstdint>
#include <cstring>
#include <string>

extern "C" int gow3gpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
extern "C" int gow3gpu_text_input_poll(char *out_utf8, uint64_t size);

namespace u2mp {
namespace {

constexpr uint64_t kClanTag = 0x11474f8 + 0x1a8;   // NetPlayerData: 8 bytes, NUL-padded
constexpr size_t kTagBytes = 8;
constexpr size_t kTagChars = 4;

constexpr uint64_t kNetSmState = 0x14ce97c;        // s32: > 1 = online
constexpr size_t kTagCompared = 4;                 // PS3 compared the first 4 bytes

bool g_editing = false;

// The tag check: the last tag checked (kept for the process) and whether a check runs.
char g_checked[kTagBytes] = {};
bool g_check_running = false;

// The censor service. Our online program has none yet (u2_online_abi.h has no comment check), so a check cannot run
// and every tag is accepted at once. A censor plugs in here: censor_start() takes the tag text (<= 63 bytes) and says
// whether a check started; censor_poll() then answers 0 while it runs, 1 accepted, -1 refused or failed.
enum class CensorStart { Unavailable, Started, Failed };
CensorStart censor_start(const char *text) {
    (void)text;
    return CensorStart::Unavailable;
}
int censor_poll() { return 1; }

// The verdict is in: the checked tag (or "*" when it was refused) is the tag, then the career and the member data
// are published.
void tag_check_done(bool rejected) {
    g_check_running = false;
    if (rejected && g_checked[0]) {
        log("clan tag: [%.4s] refused, now [*]", g_checked);
        std::memset(g_checked, 0, kTagBytes);
        g_checked[0] = '*';
    }
    std::memcpy(image.at<char>(kClanTag), g_checked, kTagBytes);
    progression_publish_career();
    online::member_data_changed();
}

// Keeps only what a tag may hold: letters (upper-cased) and digits, at most four of them.
void store_tag(const char *text, size_t size) {
    char tag[kTagBytes] = {};
    size_t taken = 0;
    for (size_t i = 0; i < size && text[i] && taken < kTagChars; ++i) {
        const char c = text[i];
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            tag[taken++] = c;
        else if (c >= 'a' && c <= 'z')
            tag[taken++] = char(c - 'a' + 'A');
    }
    std::memcpy(image.at<char>(kClanTag), tag, kTagBytes);
    log("clan tag: [%.4s]", tag);
    progression_publish();
}

}  // namespace

bool edit_clan_tag() {
    if (g_editing) return true;
    const char *stored = image.at<const char>(kClanTag);
    const std::string current(stored, strnlen(stored, kTagBytes));
    if (!gow3gpu_text_input_begin(current.c_str(), "Clan Tag (up to 4 letters or digits; empty = none)")) {
        log("clan tag: no text input available");
        return false;
    }
    g_editing = true;
    log("clan tag: text input open");
    return true;
}

void clan_tag_frame() {
    if (!g_editing) return;
    char text[64] = {};
    const int result = gow3gpu_text_input_poll(text, sizeof text);
    if (result == 0) return;   // still open
    g_editing = false;
    if (result == 1)
        store_tag(text, sizeof text);
    else
        log("clan tag: unchanged");
}

void clan_tag_check_frame() {
    if (!image.base || *image.at<const int32_t>(kNetSmState) <= 1) return;
    if (g_check_running) {
        const int verdict = censor_poll();
        if (verdict == 0) return;
        if (verdict > 0) log("clan tag: [%.4s] kept", g_checked);
        tag_check_done(verdict < 0);
        return;
    }
    const char *tag = image.at<const char>(kClanTag);
    if (std::memcmp(tag, g_checked, kTagCompared) == 0) return;
    std::memcpy(g_checked, tag, kTagBytes);
    if (!g_checked[0]) {   // cleared: published at once, nothing to check
        progression_publish_career();
        online::member_data_changed();
        return;
    }
    char text[kTagBytes + 1] = {};
    std::memcpy(text, g_checked, kTagBytes);
    switch (censor_start(text)) {
    case CensorStart::Started:
        g_check_running = true;
        log("clan tag: [%.4s] check started", g_checked);
        break;
    case CensorStart::Unavailable:   // no censor: accepted at once (never "failed", which would make every tag "*")
        log("clan tag: check unavailable, [%.4s] kept", g_checked);
        tag_check_done(false);
        break;
    case CensorStart::Failed:        // kept as typed and not checked again until it changes
        break;
    }
}

}  // namespace u2mp
