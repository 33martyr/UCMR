// u2mp scoreboard: brings back the in-match scoreboard page (PAGE_MULTIPLAYER_SCOREBOARD), the round / match
// result pages over it, and the dead player's hint line.
//
// The net player tracker overlay still owns four entries (0 the scoreboard, 1 and 3 round results, 2 results),
// but PS4 lost the code that updated and drew them, so none of them appears. We hang a host function on the
// per-frame frontend 2D draw call: after the draw it decides which entries are shown (the NGM match state, the pad
// button, the PS4 automatic test that opens the board while the local player is dead), fades and slides them like
// PS3 at the game's own frame step, asks for the menu background dim behind the result pages, runs the update,
// layout and render of the highest visible entry's page itself while no menu is up, and draws the
// "|SELECT| Scoreboard  |D-PAD| Camera" line while the local death camera runs. The page's own respawn countdown
// row (cond 44) needs nothing from us: it answers whenever the scoreboard is drawn. The call may already be wrapped
// by another module (loading.cpp), so we chain to whatever it reached before.
//
// The team lists' voice column (parity-j-voice-medals.md J4): the cell image query shows the muted icon for a muted
// player before its own talking test, in the scoreboard's lists and in the Mute Players page's (the mute toggle of
// that page is remaining.cpp, parity-n-remaining.md N2).
//
// Reimplemented from the behaviour descriptions u2mp/docs/spec/gameplay.md (1), parity-m-features.md (M1, M2,
// section 10), parity-j-voice-medals.md (J4) and parity-ticker-respawn.md (2).
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>

#include "overlay_rules.h"
#include "session_api.h"
#include "u2mp_internal.h"
#include "voice.h"


namespace u2mp {
void install_scoreboard(int &installed);

namespace {

constexpr uint64_t kCallSite = 0x3fc52d;
constexpr uint64_t kDraw2D = 0x554340;
constexpr uint64_t kPageLayout = 0x5a2670;

// Tracker overlay entry k at kTracker + 8 + 0x14 k: u8 shown, f32 fade, f32 speed, u8 dim, u32 page id.
constexpr uint64_t kTracker = 0x104f544;
constexpr uint64_t kEntryStride = 0x14;
constexpr uint64_t kTrackerShown = kTracker + 0x08;
constexpr uint64_t kTrackerFade = kTracker + 0x0c;
constexpr uint64_t kTrackerSpeed = kTracker + 0x10;
constexpr uint64_t kTrackerPage = kTracker + 0x18;
constexpr uint32_t kScoreboardPage = sid("PAGE_MULTIPLAYER_SCOREBOARD");
static_assert(kScoreboardPage == 0xf650325du, "scoreboard page id");
constexpr uint32_t kCoopRoundPage = sid("PAGE_MULTIPLAYER_ROUND_RESULTS");
constexpr uint32_t kCoopResultsPage = sid("PAGE_MULTIPLAYER_RESULTS");
constexpr uint32_t kPostgamePage = sid("PAGE_MULTIPLAYER_POSTGAME");
static_assert(kCoopRoundPage == 0x5dad5a68u && kCoopResultsPage == 0x3d437466u && kPostgamePage == 0x6b7378eau,
              "co-op result page ids");

constexpr uint32_t kEntryPage[overlay::kEntries] = {kScoreboardPage, kCoopRoundPage, kCoopResultsPage, kCoopRoundPage};
constexpr bool kEntryDim[overlay::kEntries] = {false, true, true, true};
constexpr const char *kEntryName[overlay::kEntries] = {"scoreboard", "round results", "results",
                                                       "round results (entry 3)"};

constexpr uint64_t kPadButtons = 0x14b7fb0;
constexpr uint32_t kDefaultMask = 0x100000;   // touchpad, the PS3 SELECT button's place

constexpr uint64_t kItemFade = 0x1050e58;
constexpr uint64_t kFadeScale = 0x104ed74;    // the menu's item fade target: the page item fade scale, not the HUD alpha
constexpr uint64_t kHudAlpha = 0x104ed0c;     // f32 HUD alpha (section 10.3)
constexpr uint64_t kHudManager = 0x1130008;
constexpr uint64_t kNetState = 0x14ce97c;
constexpr uint64_t kMenuPages = addr::menu_manager + 0x370;
constexpr int kMenuPageSlots = 200;
constexpr uint64_t kDimTarget = addr::menu_manager + 0x1030;      // f32 requested background dim (section 10.2)
constexpr uint64_t kDimRequested = addr::menu_manager + 0x1038;   // u8 "requested this frame"

// Clock block (sections 10.1, 10.3, 10.6).
constexpr uint64_t kClock = 0x1116180;
constexpr uint64_t kFrameStep = kClock + 0x110;    // s32 this frame's step, 1/300 s ticks
constexpr uint64_t kPauseScale = kClock + 0x4c;    // f32, 0.0 while the game is paused
constexpr uint64_t kMenuFrames = kClock + 0x104;
constexpr uint64_t kNetClock = kClock + 0x204;

// NetGameManager (section 10.7).
constexpr uint64_t kNgm = 0x113b760;
constexpr uint64_t kNgmState = kNgm + 0x1e80;
constexpr uint64_t kEndReason = kNgm + 0x1e84;
constexpr uint64_t kSubState = kNgm + 0x1e88;
constexpr uint64_t kFilmFlag = kNgm + 0x209c;
constexpr uint64_t kGameIndex = kNgm + 0x1ca0;
constexpr uint64_t kLookupPointer = 0xa15e70;
constexpr uint32_t kNetGames = sid("*net-games*");
static_assert(kNetGames == 0x3b9a067du, "net-games id");
constexpr int32_t kCinemaType = 11;

// Player trackers (ngm+0x10 + i x 0x2c0) and the local one (ngm+0), section 10.6.
constexpr int kPlayerTrackers = 10;
constexpr uint64_t kTrackerSize = 0x2c0;
constexpr uint64_t kTrackerInUse = 0x20;
constexpr uint64_t kCharacterSlot = 0x10;
constexpr uint64_t kCharacterId = 0x18;
constexpr uint64_t kFlagATime = 0x178;
constexpr uint64_t kDiedTime = 0x17c;
constexpr uint64_t kSpawnTime = 0x180;
constexpr uint64_t kFlagBTime = 0x184;

constexpr uint64_t fAutoTest = 0x9766f0;      // bool (void), PS3 0x6b6b3c (section 10.4)
constexpr uint64_t fDeathCamera = 0x664b90;   // bool (tracker *), the death camera runs (section 10.5)
constexpr uint64_t fNotice = 0x555550;        // bool (HUD manager *), a HUD notification shows (section 10.8)
constexpr uint64_t fTextFind = 0xcddc00;
constexpr uint64_t kTextDatabase = 0x1066660;
constexpr uint32_t kHintText = 0xe2186a0cu;    // "|SELECT| Scoreboard    |D-PAD| Camera"
constexpr uint64_t fSoundInit = 0x3dd780;
constexpr uint64_t fSoundPlay = 0x9b5c20;
constexpr uint32_t kOpenSound = 0xd9b465e1u;   // sound record "scoreboard-open" (dc1/ps4/audio.bin, menu.bnk)
constexpr uint32_t kCloseSound = 0x2f118f31u;  // sound record "scoreboard-close"

constexpr float kTick = 1.0f / 300.0f;
static_assert(std::bit_cast<uint32_t>(kTick) == 0x3B5A740Eu, "1/300 as a float");

template <typename T> T peek(const unsigned char *p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
template <typename T> void poke(unsigned char *p, T v) { std::memcpy(p, &v, sizeof v); }
template <typename T> T peek_image(uint64_t off) { return peek<T>(image.base + off); }
template <typename T> void poke_image(uint64_t off, T v) { poke<T>(image.base + off, v); }
const unsigned char *as_ptr(uint64_t v) { return reinterpret_cast<const unsigned char *>(uintptr_t(v)); }
bool check(uint64_t at, std::initializer_list<unsigned char> bytes) {
    return image.matches(at, bytes.begin(), bytes.size());
}

uint32_t button_mask = kDefaultMask;
uint64_t previous_target = 0;   // absolute address the call reached before we took it

// PS4 facts of parity-m-features.md section 10, each true when its verification bytes matched at install.
bool entries_ok = false;       // entries 1..3 hold the page ids the spec names: their memory is kept in step
bool ngm_ok = false;           // 10.7: states, end reason +0x1e84, sub-state +0x1e88
bool film_ok = false;          // 10.7: film flag +0x209c
bool auto_ok = false;          // 10.4: 0x9766f0
bool camera_ok = false;        // 10.5: 0x664b90
bool notice_ok = false;        // 10.8: 0x555550
bool times_ok = false;         // 10.6: tracker times +0x178..+0x184
bool record_ok = false;        // 10.4: the character's state record through its vtable +0x88
bool dim_ok = false;           // 10.2: menu +0x1030 / +0x1038
bool hud_alpha_ok = false;     // 10.3: HUD alpha 0x104ed0c
bool pause_ok = false;         // 10.3: pause scale +0x4c
bool sound_ok = false;         // 10.9: the one-shot sound pair and the two ids

overlay::Entries entries;
float fade[overlay::kEntries] = {};
unsigned char *entered_page = nullptr;
float hint = 0.0f;             // the dead player's hint line fade
bool hint_logged = false;
bool dim_logged = false;
bool film_logged = false;
bool sound_logged[2] = {};
bool auto_logged = false;      // the automatic flag as last logged
bool out_logged = false;       // "every player out" as last logged
int32_t logged_state = -1;
uint32_t previous_buttons = 0;
bool page_log_pending = true;

using LookupFn = const unsigned char *(U2MP_GUEST *)(uint32_t);

int32_t game_type() {
    const auto lookup = reinterpret_cast<LookupFn>(image.base + kLookupPointer);
    const unsigned char *table = lookup(kNetGames);
    const int16_t game = peek_image<int16_t>(kGameIndex);
    if (!table || game < 0) return -1;
    const int32_t count = peek<int32_t>(table);
    const unsigned char *records = as_ptr(peek<uint64_t>(table + 8));
    if (!records || game >= count) return -1;
    return peek<int32_t>(records + 0x90 * uint64_t(game) + 0x0c);
}

// A page among the menu manager's loaded pages (the list stops at the first NULL).
unsigned char *find_page_id(uint32_t id) {
    for (int i = 0; i < kMenuPageSlots; ++i) {
        unsigned char *page = reinterpret_cast<unsigned char *>(peek_image<uint64_t>(kMenuPages + 8ull * i));
        if (!page) break;
        if (peek<uint32_t>(page + 0x10) == id) return page;
    }
    return nullptr;
}

// The tracker's character process: slot +0x10 (holding the process), id +0x18 equal to the process's +0x44.
const unsigned char *character_of(const unsigned char *tracker) {
    const unsigned char *slot = as_ptr(peek<uint64_t>(tracker + kCharacterSlot));
    const uint32_t id = peek<uint32_t>(tracker + kCharacterId);
    if (!slot || !id) return nullptr;
    const unsigned char *process = as_ptr(peek<uint64_t>(slot));
    return process && peek<uint32_t>(process + 0x44) == id ? process : nullptr;
}

// 10.6: every player is out (threshold 0); never while the tracker fields were not verified.
bool every_player_out() {
    if (!times_ok) return false;
    using RecordFn = const unsigned char *(U2MP_GUEST *)(const unsigned char *);
    overlay::TrackerTimes t[kPlayerTrackers];
    for (int i = 0; i < kPlayerTrackers; ++i) {
        const unsigned char *tracker = image.base + kNgm + 0x10 + kTrackerSize * uint64_t(i);
        t[i].in_use = tracker[kTrackerInUse] != 0;
        if (!t[i].in_use) continue;
        t[i].died = peek<int32_t>(tracker + kDiedTime);
        t[i].spawn = peek<int32_t>(tracker + kSpawnTime);
        t[i].a_time = peek<int32_t>(tracker + kFlagATime);
        t[i].b_time = peek<int32_t>(tracker + kFlagBTime);
        const unsigned char *process = record_ok ? character_of(tracker) : nullptr;
        const unsigned char *vtable = process ? as_ptr(peek<uint64_t>(process)) : nullptr;
        const uint64_t fn = vtable ? peek<uint64_t>(vtable + 0x88) : 0;
        const unsigned char *record = fn ? reinterpret_cast<RecordFn>(uintptr_t(fn))(process) : nullptr;
        if (record) {
            t[i].flag_a = (record[0x3c] & 0x40) != 0;
            t[i].flag_b = (record[0x3d] & 0x01) != 0;
        }
    }
    return overlay::every_player_out(t, kPlayerTrackers, peek_image<int32_t>(kNetClock));
}

// M2 rule 6: PS4 0x9766f0. In a film the flag comes from the film controller, whose query call is not mapped
// (parity-m-features.md open question 15): false there.
bool automatic_flag(bool film) {
    if (film) {
        if (!film_logged) {
            film_logged = true;
            log("scoreboard: film playback: the automatic flag from the film controller (8a87d1da / f498c2b5) is not "
                "mapped, the board does not open by itself");
        }
        return false;
    }
    return auto_ok && reinterpret_cast<bool(U2MP_GUEST *)()>(image.base + fAutoTest)();
}

void play_sound(uint32_t id) {
    if (!sound_ok) return;
    alignas(16) unsigned char sound[0x200] = {};
    reinterpret_cast<void(U2MP_GUEST *)(void *, uint32_t, uint32_t)>(image.base + fSoundInit)(sound, id, 0);
    reinterpret_cast<void(U2MP_GUEST *)(void *, uint32_t)>(image.base + fSoundPlay)(sound, 0);
    bool &logged = sound_logged[id == kOpenSound ? 0 : 1];
    if (!logged) {
        logged = true;
        log("scoreboard: %s sound %08x played", id == kOpenSound ? "scoreboard-open" : "scoreboard-close", id);
    }
}

// The entry's page lays its items out again from their start positions (the PS3 setter does this whenever an
// entry's shown flag turns on).
void relayout(int k) {
    if (unsigned char *page = find_page_id(kEntryPage[k])) poke<int32_t>(page + 0xf8, 2);
    if (!page_log_pending) return;
    page_log_pending = false;
    log("scoreboard: page %08x %s", kScoreboardPage, find_page_id(kScoreboardPage) ? "found" : "NOT FOUND");
    int loaded = 0;
    for (; loaded < kMenuPageSlots; ++loaded) {
        const unsigned char *page = as_ptr(peek_image<uint64_t>(kMenuPages + 8ull * loaded));
        if (!page) break;
        const uint32_t id = peek<uint32_t>(page + 0x10);
        if (id == kCoopRoundPage) log("scoreboard: PAGE_MULTIPLAYER_ROUND_RESULTS loaded (slot %d)", loaded);
        if (id == kCoopResultsPage) log("scoreboard: PAGE_MULTIPLAYER_RESULTS loaded (slot %d)", loaded);
        if (id == kPostgamePage) log("scoreboard: PAGE_MULTIPLAYER_POSTGAME loaded (slot %d)", loaded);
    }
    log("scoreboard: %d menu pages loaded", loaded);
}

// M2 rule 4: the first request of a frame wins; the menu's own update moves its drawn dim toward it.
void request_dim(int k, float ease) {
    if (!dim_ok) return;
    if (peek_image<uint8_t>(kDimRequested) == 0) {
        poke_image<float>(kDimTarget, ease);
        poke_image<uint8_t>(kDimRequested, 1);
    }
    if (!dim_logged) {
        dim_logged = true;
        log("scoreboard: %s dims the menu background (menu +0x1030 / +0x1038)", kEntryName[k]);
    }
}

struct UpdateArgs {
    uint64_t menu;
    int32_t zero;
    float dt;
};
static_assert(sizeof(UpdateArgs) == 16, "page update arguments");

using UpdateFn = void(U2MP_GUEST *)(void *, UpdateArgs *);
using LayoutFn = void(U2MP_GUEST *)(void *, void *);
using RenderFn = void(U2MP_GUEST *)(void *, void *, void *);

void draw_page(unsigned char *page, float alpha, float x, float y, float dt) {
    const unsigned char *vptr = as_ptr(peek<uint64_t>(page));
    if (!vptr || !peek<uint64_t>(vptr + 0x10) || !peek<uint64_t>(vptr + 0x18)) return;

    if (entered_page != page) {
        entered_page = page;
        log("scoreboard: page %08x entered", peek<uint32_t>(page + 0x10));
    }

    UpdateArgs args{uint64_t(uintptr_t(image.base + addr::menu_manager)), 0, dt};
    reinterpret_cast<UpdateFn>(peek<uint64_t>(vptr + 0x10))(page, &args);

    // The draw descriptor the menu system hands to layout and render: draw context, scales,
    // alpha, offset, the 1280x720 screen rectangle and the menu time.
    alignas(8) unsigned char desc[0x50] = {};
    poke<uint64_t>(desc + 0x00, uint64_t(uintptr_t(image.base + addr::menu_manager + 0x10)));
    poke<float>(desc + 0x08, 1.0f);
    poke<float>(desc + 0x0c, 1.0f);
    poke<float>(desc + 0x10, 1.0f);
    poke<float>(desc + 0x14, 1.0f);
    poke<float>(desc + 0x18, alpha);
    poke<float>(desc + 0x24, 1.0f);
    poke<float>(desc + 0x28, x);
    poke<float>(desc + 0x2c, y);
    poke<float>(desc + 0x38, 1280.0f);
    poke<float>(desc + 0x3c, 720.0f);
    poke<float>(desc + 0x40, float(peek_image<int32_t>(kMenuFrames)) * kTick);
    poke<uint8_t>(desc + 0x44, 1);
    poke<int32_t>(desc + 0x48, peek_image<int32_t>(addr::menu_manager + 0x17a4));
    const uint32_t flags = peek<uint32_t>(page + 0xb0);
    if (flags & 0x80000) poke<float>(desc + 0x08, 1.0f * 0.75f);

    // Layout and render read the global item fade; give them this page's value and put the
    // global back afterwards so other pages are not affected.
    const float saved = peek_image<float>(kItemFade);
    const float scale = (flags & 0x800) ? 1.0f : peek_image<float>(kFadeScale);
    poke_image<float>(kItemFade, (1.0f - peek<float>(page + 0xf4)) * scale);

    reinterpret_cast<LayoutFn>(image.base + kPageLayout)(page, desc);
    reinterpret_cast<RenderFn>(peek<uint64_t>(vptr + 0x18))(page, nullptr, desc);

    poke_image<float>(kItemFade, saved);
}

// M1 3: the hint line, white, scale 0.8, centred in (0, 500, 1280, 240), drawn like the loading screen's texts.
void draw_hint(float alpha) {
    const char *text =
        reinterpret_cast<const char *(U2MP_GUEST *)(void *, uint32_t)>(image.base + fTextFind)(image.base + kTextDatabase,
                                                                                              kHintText);
    if (!text) return;
    const float xform[10] = {0.8f, 1.0f, 1.0f, 1.0f, alpha, 3.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    if (menu_text_in_rect(xform, text, 0.0f, 500.0f, 1280.0f, 240.0f) && !hint_logged) {
        hint_logged = true;
        log("scoreboard: death camera hint shown");
    }
}

using Draw2DFn = uint64_t(U2MP_GUEST *)(uint64_t, uint64_t, uint64_t);

U2MP_GUEST uint64_t frame_hook(uint64_t a, uint64_t b, uint64_t c) {
    const uint64_t result = reinterpret_cast<Draw2DFn>(previous_target)(a, b, c);

    // The game's own frame step (10.1): every rate below is r x ticks / 75.
    const float ticks = float(peek_image<int32_t>(kFrameStep));

    // Outside multiplayer (NET_SM <= 1) the rules run as NGM state 0, which hides every entry: a safety net, not PS3.
    const int32_t net_sm = peek_image<int32_t>(kNetState);
    const bool net_in = net_sm > 1;
    int32_t state = net_in ? peek_image<int32_t>(kNgmState) : 0;
    if (net_in && state != logged_state)
        log("scoreboard: NGM state %d -> %d (NET_SM %d)", int(logged_state), int(state), int(net_sm));
    logged_state = net_in ? state : -1;
    if (!ngm_ok && state != 2) state = 0;   // the state writers were not as expected: no result pages
    const int32_t type = net_in ? game_type() : -1;

    const uint32_t buttons = peek_image<uint32_t>(kPadButtons);
    const bool pressed = (buttons & ~previous_buttons & button_mask) != 0;
    previous_buttons = buttons;

    overlay::Frame f;
    f.state = state;
    f.substate = ngm_ok ? peek_image<int32_t>(kSubState) : 0;
    f.end_reason = ngm_ok ? peek_image<int32_t>(kEndReason) : 0;
    f.cinema = type == kCinemaType;
    f.out = state == 2 && every_player_out();
    if (state == 2 && !f.cinema) {
        f.auto_flag = automatic_flag(film_ok && peek_image<uint8_t>(kFilmFlag) != 0);
        f.notice = notice_ok &&
                   reinterpret_cast<bool(U2MP_GUEST *)(void *)>(image.base + fNotice)(image.base + kHudManager);
        f.pressed = pressed;
    }

    // M2 rules 6 - 8, then the entry setter's side effects: relayout, the button's sound, the entries' memory.
    bool was[overlay::kEntries];
    std::memcpy(was, entries.shown, sizeof was);
    const float speed0 = entries.speed[0];
    const overlay::FrameResult r = overlay::update_entries(entries, f);
    for (int k = 0; k < overlay::kEntries; ++k)
        if (r.relayout[k]) relayout(k);
    if (r.sound != overlay::Sound::None) play_sound(r.sound == overlay::Sound::Open ? kOpenSound : kCloseSound);
    if (entries.speed[0] != speed0) poke_image<float>(kTrackerSpeed, entries.speed[0]);

    if (state == 2 && !f.cinema && entries.auto_last != auto_logged) {
        auto_logged = entries.auto_last;
        log("scoreboard: automatic flag %s%s", auto_logged ? "on (local player dead)" : "off",
            f.auto_flag && !auto_logged ? " (a HUD notification shows)" : "");
    }
    if (state == 2 && f.out != out_logged) {
        out_logged = f.out;
        log("scoreboard: every player out: %s", out_logged ? "yes" : "no");
    }

    char reason[40];
    if (f.cinema) std::snprintf(reason, sizeof reason, "cinema, NGM state %d", int(state));
    else if (state == 2)
        std::snprintf(reason, sizeof reason, "%s", r.toggled ? "button" : f.out ? "every player out" : "automatic");
    else std::snprintf(reason, sizeof reason, "NGM state %d", int(state));

    bool any = false;
    for (int k = 0; k < overlay::kEntries; ++k) {
        const bool on = entries.shown[k];
        any = any || on;
        if (on != was[k]) {
            if (!on) entered_page = nullptr;
            if (k == 0 || entries_ok) poke_image<uint8_t>(kTrackerShown + kEntryStride * uint64_t(k), on ? 1 : 0);
            log("scoreboard: %s %s (%s)", kEntryName[k], on ? "shown" : "hidden", reason);
        }
        fade[k] = overlay::fade_step(fade[k], on, entries.speed[k], ticks);   // rule 1
        if (k == 0 || entries_ok) poke_image<float>(kTrackerFade + kEntryStride * uint64_t(k), fade[k]);
    }
    if (state == 2 || any) sync_netstat_ranks();

    // M1 2: the hint fades in while no menu is up, the local death camera runs, playing, not every player out.
    const unsigned char *local = as_ptr(peek_image<uint64_t>(kNgm));
    const bool camera = state == 2 && camera_ok && local &&
                        reinterpret_cast<bool(U2MP_GUEST *)(const unsigned char *)>(image.base + fDeathCamera)(local);
    hint = overlay::hint_step(hint, overlay::hint_wanted(menu_up(), camera, state, f.out), ticks);

    // The draw (PS3 0x4cbc10): nothing of the overlay while the game is paused.
    if (pause_ok && peek_image<float>(kPauseScale) == 0.0f) return result;
    const float hud_alpha = hud_alpha_ok ? peek_image<float>(kHudAlpha) : 1.0f;
    const float alpha = overlay::overlay_alpha(state, type, f.end_reason, hud_alpha);

    // M2 rules 2 - 4: only the highest visible entry, sliding down into place and on downward out; its dim request
    // even while a menu is up, its page only while none is (rule 2a).
    const int top = overlay::top_entry(fade);
    if (top >= 0) {
        if (kEntryDim[top]) request_dim(top, overlay::slide_ease(fade[top], entries.shown[top]));
        if (!menu_up())
            if (unsigned char *page = find_page_id(kEntryPage[top]))
                draw_page(page, alpha * fade[top], 0.0f, overlay::slide_y(fade[top], entries.shown[top]), ticks * kTick);
    }

    // M1 3: the hint line; nothing in the cinema game type.
    if (hint > 0.0f && !f.cinema) draw_hint(alpha * hint);
    return result;
}

// ---- voice column muted icon (parity-j-voice-medals.md J4) ---------------------------------------------------------
// The team list's cell image query (vtable 0x100a280 slot 15, PS4 0x5b2ac0) kept only the talking test of the voice
// column (its tracker at [rbp-0x40] at 0x5b2cb0, the tracker's key +0x23c loaded at 0x5b2cb4, then the talking
// record search). Every list of that vtable asks it: the scoreboard's team lists and the Mute Players lists.
constexpr uint64_t kTeamListVtable = 0x100a280;
constexpr unsigned kCellImageSlot = 15;
constexpr uint64_t kCellImage = 0x5b2ac0;
constexpr uint32_t kColumnTables = 0x7805beb9;      // lookup id of the team list column tables
constexpr uint64_t kNetImages = 0x1130008 + 0x70;   // the image table the cell query indexes its icons in
constexpr uint64_t kTrackerConnection = 0x23c;      // the tracker's connection key: its net id, -1 for the local player

using CellImageFn = void *(U2MP_GUEST *)(void *, const unsigned char *, int32_t, int32_t);

bool muted_icon_logged = false;

// The id of column `column` of a team list, found as the cell query finds it: the list's key (+0x190) in the sorted
// table 0x7805beb9 (16-byte entries: key, column list), then entry `column` (0x30 bytes, id at +8). 0 for none.
uint32_t column_id(const unsigned char *list, int32_t column) {
    const auto lookup = reinterpret_cast<LookupFn>(image.base + kLookupPointer);
    const unsigned char *table = lookup(kColumnTables);
    if (!table || column < 0) return 0;
    const uint32_t key = peek<uint32_t>(list + 0x190);
    const unsigned char *keys = reinterpret_cast<const unsigned char *>(peek<uint64_t>(table + 8));
    int32_t lo = 0, hi = peek<int32_t>(table) - 1;
    while (keys && lo <= hi) {
        const int32_t mid = lo + (hi - lo) / 2;
        const uint32_t k = peek<uint32_t>(keys + 16ull * uint64_t(mid));
        if (k < key) {
            lo = mid + 1;
        } else if (k > key) {
            hi = mid - 1;
        } else {
            const uint64_t at = 16ull * uint64_t(mid) + 8;
            const unsigned char *columns = reinterpret_cast<const unsigned char *>(peek<uint64_t>(keys + at));
            if (!columns || column >= peek<int32_t>(columns)) return 0;
            const unsigned char *entries = reinterpret_cast<const unsigned char *>(peek<uint64_t>(columns + 8));
            return peek<uint32_t>(entries + 0x30ull * uint64_t(column) + 8);
        }
    }
    return 0;
}

// The icon's index in the net image table (u16 count at +2, 0x28-byte entries from +8 of the list at +8), -1 when
// it is not there: the cell query stores this next to the icon id.
int32_t net_image_index(uint32_t id) {
    const unsigned char *table = reinterpret_cast<const unsigned char *>(peek_image<uint64_t>(kNetImages));
    if (!table) return -1;
    const uint16_t count = peek<uint16_t>(table + 2);
    const unsigned char *entries = reinterpret_cast<const unsigned char *>(peek<uint64_t>(table + 8));
    for (uint32_t i = 0; entries && i < count; ++i)
        if (peek<uint32_t>(entries + 8 + 0x28ull * i) == id) return int32_t(i);
    return -1;
}

// J4: the muted icon first, then the PS4 query (its talking test and every other column).
U2MP_GUEST void *cell_image_hook(void *out, const unsigned char *list, int32_t row, int32_t column) {
    const unsigned char *r = list + 0x14 * int64_t(row);
    const int32_t kind = peek<int32_t>(r + 0x48);
    const uint32_t page = peek<uint32_t>(list + 0x40);
    const uint32_t index = peek<uint32_t>(r + 0x4c);
    bool muted = false;
    if (kind == overlay::kPlayerRow && index < overlay::kTrackers) {
        const unsigned char *tracker = image.base + kNgm + 0x10 + kTrackerSize * uint64_t(index);
        muted = voice::muted(peek<uint32_t>(tracker + kTrackerConnection));   // -1 (local player): never
    }
    if (!overlay::voice_cell_muted(kind, page, index, muted ? column_id(list, column) : 0, muted))
        return reinterpret_cast<CellImageFn>(image.base + kCellImage)(out, list, row, column);
    // The image record the query gives the talking icon, with the muted icon: type 3, icon, table 1, its index.
    unsigned char *o = static_cast<unsigned char *>(out);
    const int32_t at = net_image_index(overlay::kMutedIcon);
    poke<uint32_t>(o + 0x00, 3);
    poke<uint64_t>(o + 0x08, 0);
    poke<uint32_t>(o + 0x10, 0);
    poke<uint32_t>(o + 0x14, overlay::kMutedIcon);
    poke<uint32_t>(o + 0x18, 1);
    poke<int32_t>(o + 0x1c, at);
    poke<uint32_t>(o + 0x20, 0);
    poke<int32_t>(o + 0x24, -1);
    poke<float>(o + 0x28, 1.0f);
    if (!muted_icon_logged) {
        muted_icon_logged = true;
        log("scoreboard: voice column shows the muted icon %08x (net image index %d)", overlay::kMutedIcon, int(at));
    }
    return out;
}

void install_voice_column(int &installed) {
    // J4: the cell query reads the row kind / tracker index / list key and the tracker's connection key as the hook
    // does, and the voice column id is the one it tests.
    static const unsigned char kRowKind[] = {0x41, 0x83, 0x7c, 0x86, 0x48, 0x02};         // cmp [r14+rax*4+0x48], 2
    static const unsigned char kListKey[] = {0x45, 0x8b, 0xbe, 0x90, 0x01, 0x00, 0x00};   // mov r15d, [r14+0x190]
    static const unsigned char kTables[] = {0xbf, 0xb9, 0xbe, 0x05, 0x78};                // mov edi, 0x7805beb9
    static const unsigned char kVoiceCol[] = {0x3d, 0x7b, 0x21, 0x4c, 0x00};              // cmp eax, 0x4c217b
    static const unsigned char kConnKey[] = {0x8b, 0x88, 0x3c, 0x02, 0x00, 0x00};         // mov ecx, [rax+0x23c]
    uint64_t *slot = image.at<uint64_t>(kTeamListVtable + 8ull * kCellImageSlot);
    if (image.matches(0x5b2ae2, kRowKind, sizeof kRowKind) && image.matches(0x5b2b65, kListKey, sizeof kListKey) &&
        image.matches(0x5b2b6c, kTables, sizeof kTables) && image.matches(0x5b2c3b, kVoiceCol, sizeof kVoiceCol) &&
        image.matches(0x5b2cb4, kConnKey, sizeof kConnKey) && *slot == uint64_t(uintptr_t(image.base + kCellImage))) {
        *slot = uint64_t(uintptr_t(&cell_image_hook));
        installed += 1;
        log("scoreboard: team list cell image 0x%llx -> C++ (voice column muted icon, PS3 parity J4)",
            (unsigned long long)kCellImage);
    } else {
        log("scoreboard: team list cell image query not as expected: no muted icon in the voice column");
    }
}


// The PS4 facts of parity-m-features.md section 10 the overlay uses, each with its verification bytes; a part whose
// bytes differ stays off (logged) and the overlay keeps working without it.
void check_facts() {
    entries_ok = true;
    for (int k = 1; k < overlay::kEntries; ++k)
        entries_ok = entries_ok && peek_image<uint32_t>(kTrackerPage + kEntryStride * uint64_t(k)) == kEntryPage[k];
    if (!entries_ok) log("scoreboard: tracker entries 1-3 not as expected, their memory is left alone");

    // 10.7: end reason, sub-state (and its state-4 rule), the round start, match end, leave and exit writers.
    ngm_ok = check(0x64a7c7, {0x89, 0xb7, 0x84, 0x1e, 0x00, 0x00}) &&
             check(0x5e53f0, {0x89, 0x88, 0x88, 0x1e, 0x00, 0x00}) &&
             check(0x5e5409, {0x83, 0xf9, 0x03, 0x75, 0x0b, 0xc7, 0x80, 0x80, 0x1e, 0x00, 0x00, 0x04, 0x00, 0x00,
                              0x00}) &&
             check(0x647a82, {0x48, 0xc7, 0x83, 0x80, 0x1e, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
                              0xc7, 0x83, 0x88, 0x1e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}) &&
             check(0x648c65, {0x41, 0xc7, 0x87, 0x80, 0x1e, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00}) &&
             check(0x641f63, {0x41, 0xc7, 0x84, 0x24, 0x80, 0x1e, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00}) &&
             check(0x5e8907, {0x41, 0xc7, 0x86, 0x80, 0x1e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    if (!ngm_ok) log("scoreboard: NGM state writers not as expected: no result pages, end reason / sub-state read as 0");
    film_ok = check(0x577f9f, {0x80, 0xb8, 0x9c, 0x20, 0x00, 0x00, 0x00});
    if (!film_ok) log("scoreboard: film flag test 0x577f9f not as expected: film playback not detected");

    // 10.4 (the prologue alone also matches its sibling 0x9765d0, so all three).
    auto_ok = check(0x9766f0, {0x55, 0x48, 0x89, 0xe5, 0x53, 0x48, 0x81, 0xec, 0x88, 0x00, 0x00, 0x00}) &&
              check(0x9767c9, {0xbe, 0xee, 0x88, 0x96, 0x1a, 0xe8, 0xbd, 0xd8, 0xb4, 0xff, 0xb0, 0x01, 0x83, 0x7d, 0xe8,
                               0x02, 0x72, 0x12}) &&
              check(0x9767e4, {0x8a, 0x40, 0x3c, 0x24, 0x01});
    if (!auto_ok) log("scoreboard: automatic test 0x9766f0 not as expected: the board does not open by itself");
    record_ok = check(0x65b902, {0x48, 0x8b, 0x07, 0xff, 0x90, 0x88, 0x00, 0x00, 0x00, 0x8a, 0x48, 0x3d, 0x80, 0xe1,
                                 0x01});
    if (!record_ok) log("scoreboard: state record call 0x65b902 not as expected: down flags A / B not read");
    times_ok = check(0x6626b5, {0x48, 0xc7, 0x83, 0x80, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                0x48, 0xc7, 0x83, 0x78, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    if (!times_ok) log("scoreboard: tracker reset 0x6626b5 not as expected: \"every player out\" never holds");

    // 10.5, 10.8.
    camera_ok = check(0x664b90, {0x48, 0x8b, 0x87, 0x68, 0x01, 0x00, 0x00, 0x48, 0x85, 0xc0, 0x74, 0x19, 0x8b, 0x8f,
                                 0x70, 0x01, 0x00, 0x00, 0x85, 0xc9, 0x74, 0x12, 0x48, 0x8b, 0x00, 0x48, 0x85, 0xc0,
                                 0x74, 0x0d, 0x39, 0x48, 0x44, 0x0f, 0x94, 0xc0});
    if (!camera_ok) log("scoreboard: death camera test 0x664b90 not as expected: no hint line");
    notice_ok = check(0x555550, {0x48, 0x8b, 0x8f, 0x10, 0x03, 0x00, 0x00, 0x48, 0x8b, 0x91, 0x38, 0x01, 0x00, 0x00,
                                 0x48, 0x85, 0xd2, 0x74, 0x07, 0xb0, 0x01, 0x83, 0x3a, 0x00, 0x75, 0x15, 0x48, 0x8b,
                                 0x89, 0x40, 0x01, 0x00, 0x00, 0x48, 0x85, 0xc9, 0x74, 0x07, 0xb0, 0x01, 0x83, 0x39,
                                 0x00, 0x75, 0x02, 0x31, 0xc0, 0xc3});
    if (!notice_ok) log("scoreboard: HUD notification test 0x555550 not as expected: no notification override");

    // 10.2: the menu update clears the request byte and moves its dim; the menu's own setter pattern.
    dim_ok = check(0x5d06a4, {0x41, 0xc6, 0x85, 0x38, 0x10, 0x00, 0x00}) &&
             check(0x5d0704, {0xc4, 0xc1, 0x7a, 0x10, 0x8d, 0x30, 0x10, 0x00, 0x00, 0xc4, 0xc1, 0x7a, 0x10, 0x85, 0x34,
                              0x10, 0x00, 0x00}) &&
             check(0x5d1afd, {0x80, 0xbb, 0x38, 0x10, 0x00, 0x00, 0x00, 0x75, 0x0f, 0xc5, 0xfa, 0x11, 0x83, 0x30, 0x10,
                              0x00, 0x00, 0xc6, 0x83, 0x38, 0x10, 0x00, 0x00, 0x01});
    if (!dim_ok) log("scoreboard: menu background dim fields not as expected: result pages not dimmed");

    // 10.3.
    hud_alpha_ok = check(0x555cd1, {0x48, 0x8d, 0x05, 0x34, 0x90, 0xaf, 0x00, 0xc4, 0xe2, 0x79, 0x18, 0x30});
    if (!hud_alpha_ok) log("scoreboard: HUD alpha read 0x555cd1 not as expected: overlay alpha 1.0");
    pause_ok = check(0x3faf57, {0x49, 0xff, 0x47, 0x28, 0xc5, 0xf8, 0x57, 0xc0, 0xc4, 0xc1, 0x78, 0x2e, 0x47, 0x4c,
                                0x74, 0x04});
    if (!pause_ok) log("scoreboard: pause test 0x3faf57 not as expected: the overlay also draws while paused");

    // 10.9: the one-shot sound calls and the uncalled PS4 entry setter that names the two ids.
    sound_ok = check(fSoundInit, {0x55, 0x48, 0x89, 0xe5}) && check(fSoundPlay, {0x55, 0x48, 0x89, 0xe5}) &&
               check(0x666b8b, {0x85, 0xf6, 0x75, 0x2f, 0x80, 0xf1, 0x01, 0x75, 0x2a, 0x84, 0xdb, 0xb8, 0xe1, 0x65,
                                0xb4, 0xd9, 0xbe, 0x31, 0x8f, 0x11, 0x2f});
    if (!sound_ok) log("scoreboard: one-shot sound calls / entry setter not as expected, no open / close sounds");
}

}  // namespace

void install_scoreboard(int &installed) {
    install_voice_column(installed);   // the team lists' voice column muted icon, also without the overlay
    if (const char *env = std::getenv("UNCHARTED_U2MP_SCOREBOARD_BUTTON")) {
        // Parsed as a 32-bit value on every platform: overflow saturates to 0xffffffff.
        const unsigned long long v = std::strtoull(env, nullptr, 16);
        button_mask = v > 0xffffffffull ? 0xffffffffu : uint32_t(v);
        if (button_mask == 0) {
            log("scoreboard: off (UNCHARTED_U2MP_SCOREBOARD_BUTTON=0)");
            return;
        }
    }
    if (!check(kPageLayout, {0x55, 0x48, 0x89, 0xE5}) || !check(kDraw2D, {0x55, 0x48, 0x89, 0xE5})) {
        log("scoreboard: page layout / 2D draw not as expected, not installed");
        return;
    }
    if (peek_image<uint32_t>(kTrackerPage) != kScoreboardPage || peek_image<float>(kTrackerSpeed) != 1.0f) {
        log("scoreboard: tracker 0x%llx is not as expected, not installed", (unsigned long long)kTracker);
        return;
    }
    if (!image.executable(kCallSite, 5) || image.base[kCallSite] != 0xE8) {
        log("scoreboard: 0x%llx is not a call, not installed", (unsigned long long)kCallSite);
        return;
    }
    // 10.1: lea of the clock block, int-to-float of its frame step. Without it no rate is known.
    if (!check(0x5a3c74, {0x48, 0x8d, 0x05, 0x05, 0x25, 0xb7, 0x00, 0xc5, 0xfa, 0x2a, 0x88, 0x10, 0x01, 0x00, 0x00})) {
        log("scoreboard: frame step read 0x5a3c74 not as expected, not installed");
        return;
    }
    check_facts();

    const int32_t rel = peek_image<int32_t>(kCallSite + 1);
    const uint64_t current = uint64_t(int64_t(kCallSite + 5) + rel);   // may lie outside the image
    previous_target = uint64_t(uintptr_t(image.base)) + current;
    const char *name = current == kDraw2D ? "scoreboard: 2D draw" : "scoreboard: 2D draw (chained)";
    if (!retarget_call(kCallSite, current, reinterpret_cast<void *>(&frame_hook), name)) return;
    installed += 1;
    log("scoreboard: PAGE_MULTIPLAYER_SCOREBOARD port installed (button mask 0x%x; PS3 0x4cbf00 / 0x4cbc10; "
        "auto open %s, hint %s, every-player-out %s, dim %s, sounds %s, result states %s)",
        button_mask, auto_ok ? "on" : "off", camera_ok ? "on" : "off", times_ok ? "on" : "off", dim_ok ? "on" : "off",
        sound_ok ? "on" : "off", ngm_ok ? "on" : "off");
}

}  // namespace u2mp
