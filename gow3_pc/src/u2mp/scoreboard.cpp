// u2mp scoreboard: brings back the in-match scoreboard page (PAGE_MULTIPLAYER_SCOREBOARD), the round / match
// result pages over it, and the dead player's hint line.
//
// The net player tracker overlay still owns four entries (0 the scoreboard, 1 and 3 round results, 2 results),
// but PS4 lost the code that updated and drew them, so none of them appears. We hang a host function on the
// per-frame frontend 2D draw call: after the draw it decides which entries are shown (pad button, the local player
// being dead, the round / match end), fades and slides them like PS3, runs the update, layout and render of the
// highest visible entry's page itself, and draws the "|SELECT| Scoreboard  |D-PAD| Camera" line while the local
// player is dead. The page's own respawn countdown row (cond 44) needs nothing from us: it answers whenever the
// scoreboard is drawn. The call may already be wrapped by another module (loading.cpp), so we chain to whatever
// it reached before.
//
// The team lists' voice column (parity-j-voice-medals.md J4): the cell image query shows the muted icon for a muted
// player before its own talking test, in the scoreboard's lists and in the Mute Players page's (the mute toggle of
// that page is remaining.cpp, parity-n-remaining.md N2).
//
// Reimplemented from the behaviour descriptions u2mp/docs/spec/gameplay.md (1), parity-m-features.md (M1, M2),
// parity-j-voice-medals.md (J4) and parity-ticker-respawn.md (2).
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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
constexpr uint64_t kFadeScale = 0x104ed74;
constexpr uint64_t kNetState = 0x14ce97c;
constexpr uint64_t kNgmState = 0x113b760 + 0x1e80;
constexpr uint64_t kMenuFrames = 0x1116180 + 0x104;
constexpr uint64_t kMenuPages = addr::menu_manager + 0x370;
constexpr int kMenuPageSlots = 200;

constexpr uint64_t kNgm = 0x113b760;
constexpr uint64_t kGameIndex = kNgm + 0x1ca0;
constexpr uint64_t kRoundsPlayed = kNgm + 0x1d74;
constexpr uint64_t kRoundLimit = kNgm + 0x21c0;
constexpr uint64_t kLookupPointer = 0xa15e70;
constexpr uint32_t kNetGames = sid("*net-games*");
static_assert(kNetGames == 0x3b9a067du, "net-games id");
constexpr float kRoundResultsSeconds = 12.0f;   // gone ~8 s before the next objective message

// Player trackers (ngm+0x10 + i x 0x2c0) and the local one (ngm+0): name, character handle, died / spawned times.
constexpr uint64_t kTrackerSize = 0x2c0;
constexpr uint64_t kTrackerName = 0x20;
constexpr uint64_t kCharacterSlot = 0x10;
constexpr uint64_t kCharacterId = 0x18;
constexpr uint64_t kDiedTime = 0x17c;
constexpr uint64_t kSpawnTime = 0x180;

constexpr uint64_t fTextFind = 0xcddc00;
constexpr uint64_t kTextDatabase = 0x1066660;
constexpr uint32_t kHintText = 0xe2186a0cu;    // "|SELECT| Scoreboard    |D-PAD| Camera"
constexpr uint64_t fSoundInit = 0x3dd780;
constexpr uint64_t fSoundPlay = 0x9b5c20;
constexpr uint32_t kOpenSound = 0xd9b465e1u;
constexpr uint32_t kCloseSound = 0x2f118f31u;
constexpr uint64_t fRespawnQuery = 0x65b830;   // cond 44: its field reads are the dead test used here

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

uint32_t button_mask = kDefaultMask;
uint64_t previous_target = 0;   // absolute address the call reached before we took it

bool entries_ok = false;   // entries 1..3 hold the page ids the spec names: their memory is kept in step
bool auto_ok = false;      // the cond 44 query is as the spec read it: the dead test may use its fields
bool sound_ok = false;     // the one-shot sound pair starts as expected
bool state_rules = false;  // UNCHARTED_U2MP_SCOREBOARD_STATES=1: result pages from NGM states 3 / 4 / 5

bool shown[overlay::kEntries] = {};
float fade[overlay::kEntries] = {};
float speed[overlay::kEntries] = {1.0f, 1.0f, 1.0f, 1.0f};
unsigned char *entered_page = nullptr;
bool board = false;       // entry 0 wanted (button or the automatic rule)
bool by_button = false;   // ... and the player opened it with the button
bool auto_last = false;   // the automatic flag of the previous frame
int request_entry = 0;    // the entry the round / match result timers show
float hint = 0.0f;        // the dead player's hint line fade
bool hint_logged = false;
bool dim_logged = false;
bool sound_logged[2] = {};
bool in_match = false;    // the NGM state was "playing" since the player entered the match
int32_t last_ngm_state = -1;
bool state_skip_logged[overlay::kEntries] = {};
uint32_t previous_buttons = 0;
bool first_frame = true;
std::chrono::steady_clock::time_point last_time;
bool page_log_pending = true;

float auto_left = 0.0f;   // seconds the round/match result board stays up by itself
float end_hold = 0.0f;    // after the match ends the game leaves at once; keep the results up
int32_t last_end = 0;

using LookupFn = const unsigned char *(U2MP_GUEST *)(uint32_t);

int32_t game_type() {
    const auto lookup = reinterpret_cast<LookupFn>(image.base + kLookupPointer);
    const unsigned char *table = lookup(kNetGames);
    const int16_t game = peek_image<int16_t>(kGameIndex);
    if (!table || game < 0) return -1;
    const int32_t count = peek<int32_t>(table);
    const unsigned char *records = reinterpret_cast<const unsigned char *>(peek<uint64_t>(table + 8));
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

// The overlay entry that draws `page` when a result timer asks for it.
int entry_of_page(uint32_t page) {
    if (page == kCoopRoundPage) return 1;
    if (page == kCoopResultsPage) return 2;
    return 0;
}

// The tracker's character is gone, or it died after its last spawn (the cond 44 dead test, parity-ticker-respawn.md
// 2.3 rule 2, without the two state-record flags whose PS4 call is not mapped).
bool out_of_play(const unsigned char *tracker) {
    const unsigned char *slot = reinterpret_cast<const unsigned char *>(peek<uint64_t>(tracker + kCharacterSlot));
    const uint32_t id = peek<uint32_t>(tracker + kCharacterId);
    const unsigned char *character = slot && id ? reinterpret_cast<const unsigned char *>(peek<uint64_t>(slot)) : nullptr;
    if (!character || peek<uint32_t>(character + 0x44) != id) return true;
    return peek<int32_t>(tracker + kDiedTime) >= peek<int32_t>(tracker + kSpawnTime);
}

bool local_dead() {
    const unsigned char *local = reinterpret_cast<const unsigned char *>(peek_image<uint64_t>(kNgm));
    return local && out_of_play(local);
}

// Every tracker in use (a name) is out of play (M1 2, last point).
bool every_player_out() {
    bool any = false;
    for (int i = 0; i < 10; ++i) {
        const unsigned char *tracker = image.base + kNgm + 0x10 + kTrackerSize * uint64_t(i);
        if (tracker[kTrackerName] == 0) continue;
        any = true;
        if (!out_of_play(tracker)) return false;
    }
    return any;
}

void play_sound(uint32_t id) {
    if (!sound_ok) return;
    alignas(16) unsigned char sound[0x200] = {};
    reinterpret_cast<void(U2MP_GUEST *)(void *, uint32_t, uint32_t)>(image.base + fSoundInit)(sound, id, 0);
    reinterpret_cast<void(U2MP_GUEST *)(void *, uint32_t)>(image.base + fSoundPlay)(sound, 0);
    bool &logged = sound_logged[id == kOpenSound ? 0 : 1];
    if (!logged) {
        logged = true;
        log("scoreboard: %s sound %08x played (its PS4 bank is not confirmed: silent if no bank has it)",
            id == kOpenSound ? "open" : "close", id);
    }
}

void set_entry(int k, bool on, const char *reason) {
    if (on == shown[k]) return;
    shown[k] = on;
    if (!on) entered_page = nullptr;
    if (k == 0 || entries_ok) poke_image<uint8_t>(kTrackerShown + kEntryStride * uint64_t(k), on ? 1 : 0);
    if (on) {
        // Make the page lay its items out again from their start positions.
        if (unsigned char *page = find_page_id(kEntryPage[k])) poke<int32_t>(page + 0xf8, 2);
        if (page_log_pending) {
            page_log_pending = false;
            log("scoreboard: page %08x %s", kScoreboardPage, find_page_id(kScoreboardPage) ? "found" : "NOT FOUND");
            for (int i = 0; i < kMenuPageSlots; ++i) {
                const unsigned char *page = reinterpret_cast<const unsigned char *>(peek_image<uint64_t>(kMenuPages + 8ull * i));
                if (!page) break;
                const uint32_t id = peek<uint32_t>(page + 0x10);
                if (id == kCoopRoundPage) log("scoreboard: PAGE_MULTIPLAYER_ROUND_RESULTS loaded (slot %d)", i);
                if (id == kCoopResultsPage) log("scoreboard: PAGE_MULTIPLAYER_RESULTS loaded (slot %d)", i);
                if (id == kPostgamePage) log("scoreboard: PAGE_MULTIPLAYER_POSTGAME loaded (slot %d)", i);
            }
            int loaded = 0;
            while (loaded < kMenuPageSlots && peek_image<uint64_t>(kMenuPages + 8ull * loaded)) ++loaded;
            log("scoreboard: %d menu pages loaded", loaded);
        }
    }
    log("scoreboard: %s %s (%s)", kEntryName[k], on ? "shown" : "hidden", reason);
}

void set_speed0(float v) {
    speed[0] = v;
    poke_image<float>(kTrackerSpeed, v);
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
    const unsigned char *vptr = reinterpret_cast<const unsigned char *>(peek<uint64_t>(page));
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

    const auto now = std::chrono::steady_clock::now();
    float dt = 0.0f;
    if (!first_frame) {
        // Single precision throughout: nanoseconds as a float, divided by 1e9.
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - last_time).count();
        dt = float(ns) / 1e9f;
        if (dt > 0.1f) dt = 0.1f;   // no big jump after a hitch or a pause
    }
    first_frame = false;
    last_time = now;

    const int32_t net_sm = peek_image<int32_t>(kNetState);
    const int32_t ngm_state = peek_image<int32_t>(kNgmState);
    const bool net_in = net_sm > 1;
    const bool playing = net_in && ngm_state == 2;
    if (net_in && ngm_state != last_ngm_state && (in_match || playing))
        log("scoreboard: NGM state %d -> %d (NET_SM %d)%s", int(last_ngm_state), int(ngm_state), int(net_sm),
            ngm_state >= 3 && ngm_state <= 5 && !state_rules
                ? "; state result pages off (UNCHARTED_U2MP_SCOREBOARD_STATES=1 turns them on)"
                : "");
    last_ngm_state = net_in ? ngm_state : -1;
    if (playing) in_match = true;
    else if (!net_in || ngm_state < 2 || ngm_state > 5) in_match = false;
    const int32_t type = in_match ? game_type() : -1;
    const bool cinema = type == 11;

    const uint32_t buttons = peek_image<uint32_t>(kPadButtons);
    const bool pressed = (buttons & ~previous_buttons & button_mask) != 0;
    previous_buttons = buttons;

    // The round-end flag comes up while Playing is still the state; show the board then and take
    // it down as soon as the next round clears the flag. A final round keeps the match results up.
    if (playing) {
        const int32_t end = peek_image<int32_t>(kNgm + 0x1e84);
        const int32_t rounds = peek_image<int32_t>(kRoundsPlayed);
        const int32_t limit = peek_image<int32_t>(kRoundLimit);
        const bool round_game = type == 0 || type == 4 || type == 5 || type == 6 || type == 7 || type == 8 ||
                                type == 9 || type == 10 || type == 12;
        if (round_game && end != 0 && last_end == 0) {
            uint32_t page;
            const bool final_round = limit <= 0 || rounds + 1 >= limit;
            if (final_round) {
                const bool scoreboard_page = find_page_id(kScoreboardPage) != nullptr;
                const bool results = find_page_id(kCoopResultsPage) != nullptr;
                page = scoreboard_page ? kScoreboardPage : (results ? kCoopResultsPage : kScoreboardPage);
                auto_left = kRoundResultsSeconds;
                end_hold = kRoundResultsSeconds;
                log("scoreboard: match results page %08x %s (full %s, summary %s)", page,
                    find_page_id(page) ? "selected" : "missing", scoreboard_page ? "loaded" : "missing",
                    results ? "loaded" : "missing");
            } else {
                const bool round_mode = type == 4 || type == 5 || type == 6 || type == 7 || type == 8 ||
                                        type == 9 || type == 12;
                const bool round_page = round_mode && find_page_id(kCoopRoundPage) != nullptr;
                const bool results_page = find_page_id(kCoopResultsPage) != nullptr;
                const bool scoreboard_page = find_page_id(kScoreboardPage) != nullptr;
                if (round_page) page = kCoopRoundPage;
                else if (type == 10 && scoreboard_page) page = kScoreboardPage;
                else page = results_page ? kCoopResultsPage : kScoreboardPage;
                auto_left = 60.0f;   // until the next round clears the flag
                log("scoreboard: round %d results page %08x %s", int(rounds + 1), page,
                    find_page_id(page) ? "selected" : "missing");
            }
            request_entry = entry_of_page(page);
        }
        if (end == 0 && last_end != 0) {
            auto_left = 0.0f;
            end_hold = 0.0f;
            log("scoreboard: round started, board hidden");
        }
        last_end = end;
    } else if (end_hold > 0.0f) {
        end_hold -= dt;   // match over, the game is leaving: let the results board finish
    } else {
        auto_left = 0.0f;
        last_end = 0;
    }
    if (auto_left > 0.0f) auto_left -= dt;

    // M2 rule 6: the board opens by itself while the local player is dead, closes when the player is back
    // (unless the player opened it with the button). No sound.
    const bool dead = playing && auto_ok && local_dead();
    switch (overlay::auto_edge(auto_last, dead && !cinema)) {
    case overlay::Edge::Opened:
        if (!board) {
            board = true;
            by_button = false;
            set_speed0(1.0f);
        }
        log("scoreboard: local player dead, board opens by itself");
        break;
    case overlay::Edge::Closed:
        if (!by_button) board = false;
        if (playing) log("scoreboard: local player back%s", by_button ? " (board kept: opened by the button)" : "");
        break;
    case overlay::Edge::None:
        break;
    }

    // M2 rule 7: the button toggles the board (speed 2.0, with a sound).
    if (!playing && end_hold <= 0.0f) {
        board = false;
        by_button = false;
    } else if (pressed) {
        const bool open = !(board || auto_left > 0.0f);
        board = open;
        by_button = open;
        auto_left = 0.0f;
        set_speed0(2.0f);
        play_sound(open ? kOpenSound : kCloseSound);
    }

    // M2 rule 8 (opt-in): NGM states 3 / 4 / 5 show one result entry and clear the rest; the result timers above
    // (the stand-in for PS3's request bytes) come first.
    int state = state_rules && in_match && !playing ? overlay::state_entry(ngm_state, -1, cinema) : -1;
    if (state >= 0 && !find_page_id(kEntryPage[state])) {
        if (!state_skip_logged[state]) {
            state_skip_logged[state] = true;
            log("scoreboard: NGM state %d wants the %s page %08x, not loaded: nothing shown for it", int(ngm_state),
                kEntryName[state], kEntryPage[state]);
        }
        state = -1;
    }
    if (state >= 0 && auto_left <= 0.0f) {
        board = false;
        by_button = false;
    }

    const int only = auto_left > 0.0f ? request_entry : state;
    const char *reason = (!playing && end_hold <= 0.0f && state < 0) ? "not playing"
                         : auto_left > 0.0f                          ? "round results"
                         : state >= 0                                ? "match state"
                         : pressed                                   ? "button"
                         : board                                     ? "local player dead"
                                                                     : "automatic";
    bool any = false;
    for (int k = 0; k < overlay::kEntries; ++k) {
        const bool want = only >= 0 ? k == only : (k == 0 && board);
        any = any || want;
        set_entry(k, want, reason);
        fade[k] = overlay::fade_step(fade[k], shown[k], speed[k], dt);
        if (k == 0 || entries_ok) poke_image<float>(kTrackerFade + kEntryStride * uint64_t(k), fade[k]);
    }
    if (playing || any) sync_netstat_ranks();

    // M2 rules 2 and 3: only the highest visible entry draws, sliding down into place and on downward out.
    const int top = overlay::top_entry(fade);
    if (top >= 0) {
        if (unsigned char *page = find_page_id(kEntryPage[top])) {
            if (kEntryDim[top] && !dim_logged) {
                dim_logged = true;
                log("scoreboard: %s wants the menu background dim (PS3 menu +0x5d0 / +0x5d8); its PS4 fields are "
                    "not mapped, nothing dimmed", kEntryName[top]);
            }
            draw_page(page, fade[top], 0.0f, overlay::slide_y(fade[top], shown[top]), dt);
        }
    }

    // M1: the hint line while the local player is dead (the death camera runs then) and not every player is out.
    hint = overlay::hint_step(hint, dead && !every_player_out(), dt);
    if (hint > 0.0f && !cinema) draw_hint(hint);
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

}  // namespace

void install_scoreboard(int &installed) {
    install_voice_column(installed);   // the team lists' voice column muted icon, also without the overlay
    if (const char *env = std::getenv("UNCHARTED_U2MP_SCOREBOARD_BUTTON")) {
        // The runtime is Windows x64, where unsigned long is 32 bits: overflow saturates to 0xffffffff.
        button_mask = uint32_t(std::strtoul(env, nullptr, 16));
        if (button_mask == 0) {
            log("scoreboard: off (UNCHARTED_U2MP_SCOREBOARD_BUTTON=0)");
            return;
        }
    }
    static const unsigned char prologue[] = {0x55, 0x48, 0x89, 0xE5};
    if (!image.matches(kPageLayout, prologue, sizeof prologue) || !image.matches(kDraw2D, prologue, sizeof prologue)) {
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

    entries_ok = true;
    for (int k = 1; k < overlay::kEntries; ++k)
        entries_ok = entries_ok && peek_image<uint32_t>(kTrackerPage + kEntryStride * uint64_t(k)) == kEntryPage[k];
    if (!entries_ok) log("scoreboard: tracker entries 1-3 not as expected, their memory is left alone");

    static const unsigned char kQuery[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                           0x53, 0x48, 0x81, 0xec, 0x88, 0x00, 0x00, 0x00, 0x48, 0x89, 0xfb, 0x4c};
    static const unsigned char kQueryOp[] = {0x83, 0x3b, 0x01, 0x0f, 0x85, 0xf1, 0x02, 0x00, 0x00};
    auto_ok = image.matches(fRespawnQuery, kQuery, sizeof kQuery) && image.matches(0x65b855, kQueryOp, sizeof kQueryOp);
    if (!auto_ok) log("scoreboard: cond 44 query 0x%llx not as expected: no automatic board or hint line on death",
                      (unsigned long long)fRespawnQuery);
    sound_ok = image.matches(fSoundInit, prologue, sizeof prologue) && image.matches(fSoundPlay, prologue, sizeof prologue);
    if (!sound_ok) log("scoreboard: one-shot sound calls not as expected, no open / close sounds");
    const char *states = std::getenv("UNCHARTED_U2MP_SCOREBOARD_STATES");
    state_rules = states && std::strcmp(states, "1") == 0;

    const int32_t rel = peek_image<int32_t>(kCallSite + 1);
    const uint64_t current = uint64_t(int64_t(kCallSite + 5) + rel);   // may lie outside the image
    previous_target = uint64_t(uintptr_t(image.base)) + current;
    const char *name = current == kDraw2D ? "scoreboard: 2D draw" : "scoreboard: 2D draw (chained)";
    if (!retarget_call(kCallSite, current, reinterpret_cast<void *>(&frame_hook), name)) return;
    installed += 1;
    log("scoreboard: PAGE_MULTIPLAYER_SCOREBOARD port installed (button mask 0x%x; PS3 0x4cbf00 / 0x4cbc10; "
        "auto open %s, sounds %s, state result pages %s)",
        button_mask, auto_ok ? "on" : "off", sound_ok ? "on" : "off", state_rules ? "on" : "off");
}

void show_round_results() {
    const bool full = find_page_id(kScoreboardPage) != nullptr;
    const bool results = find_page_id(kCoopResultsPage) != nullptr;
    const bool round_page = find_page_id(kCoopRoundPage) != nullptr;
    uint32_t page = full ? kScoreboardPage : (results ? kCoopResultsPage : (round_page ? kCoopRoundPage : kScoreboardPage));
    if (!find_page_id(page)) page = kScoreboardPage;
    request_entry = entry_of_page(page);
    auto_left = kRoundResultsSeconds;
    end_hold = kRoundResultsSeconds;
    log("scoreboard: round results requested (%08x%s)", page, find_page_id(page) ? "" : ", page missing");
}

}  // namespace u2mp
