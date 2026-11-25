// u2mp: the pure rules of the in-match overlay (scoreboard / result entries, dead-player hint line) and of the
// PAGE_MULTIPLAYER stats ticker. No game memory is touched here except the ticker object bytes passed in, so the
// rules can be tested without the game (tests/test_u2mp_overlay.cpp). Used by scoreboard.cpp and ticker.cpp.
//
// Reimplemented from the behaviour descriptions u2mp/docs/spec/parity-m-features.md (M1, M2) and
// u2mp/docs/spec/parity-ticker-respawn.md (1.2 - 1.6).
#pragma once

#include <cstdint>

namespace u2mp::overlay {

// ---- scoreboard / result entries (parity-m-features.md M2) ----

constexpr int kEntries = 4;   // 0 scoreboard, 1 round results, 2 results, 3 round results
constexpr float kSlide = 20.0f;

// Rule 1: the entry's fade moves toward its shown flag at `speed` per second, clamped to 0..1.
float fade_step(float fade, bool shown, float speed, float dt);
// Rule 2: the highest entry whose fade is above 0 is the one drawn; -1 when none is.
int top_entry(const float (&fade)[kEntries]);
// Rule 3: fading in, ease = 1 - (1 - fade)^2 and the page sits 20 x (1 - ease) above its place (y < 0);
// fading out, ease = fade^2 and it sits 20 x (1 - ease) below (y > 0). The dim value of rule 4 is this ease.
float slide_ease(float fade, bool shown);
float slide_y(float fade, bool shown);
// Rule 8: the entry the NGM match state shows on its own (every other entry cleared), -1 for none (state 2 and
// every other state). `round_substate` is the NGM round sub-state, -1 while its PS4 field is not known (state 5
// then shows the scoreboard, as PS3 does for every sub-state but 3).
int state_entry(int32_t ngm_state, int32_t round_substate, bool cinema);

// Rule 6: the automatic flag's change since the last frame; `last` is updated.
enum class Edge { None, Opened, Closed };
Edge auto_edge(bool &last, bool now);

// ---- dead-player hint line (M1) ----

constexpr float kHintIn = 1.0f;    // per second, while every condition of M1 2 holds
constexpr float kHintOut = 2.0f;   // per second, otherwise
float hint_step(float h, bool on, float dt);

// ---- PAGE_MULTIPLAYER stats ticker object (parity-ticker-respawn.md 1.2), offsets from the object ----

constexpr uint32_t kTickerWidth = 0x04;      // f32 strip width
constexpr uint32_t kTickerDrawn = 0x10;      // f32 time of the last scroll-and-draw, seconds
constexpr uint32_t kTickerEntry = 0x14;      // entry k at kTickerEntry + k x kTickerStride: f32 start, f32 width, text
constexpr uint32_t kTickerStride = 0x208;
constexpr uint32_t kTickerTextOff = 0x08;    // the text within an entry (first byte 0 = entry free)
constexpr uint32_t kTickerTextMax = 0x200;   // bytes including the NUL
constexpr int kTickerSlots = 8;
constexpr uint32_t kTickerSize = kTickerEntry + kTickerSlots * kTickerStride;
static_assert(kTickerSize == 0x1054, "ticker object size: the last entry's text ends at 0x1054");
constexpr float kTickerEdgeGap = 10.0f;      // a new text enters this far right of the strip's right edge
constexpr float kTickerQueueGap = 150.0f;    // or this far behind the last text still coming in
constexpr double kTickerVisibleSeconds = 0.1;

// 1.4: where the next added text starts (relative to the strip x).
float ticker_next_start(const unsigned char *ticker);
// 1.4: the entry the next text goes into (the lowest free one), -1 when all 8 are in use.
int ticker_free_entry(const unsigned char *ticker);
// 1.5: every queued text plus its 150-unit gap has fully come in from the right edge (true when empty).
bool ticker_finished(const unsigned char *ticker);
// 1.6: the strip was drawn within the last 0.1 s; `frame_clock` is the s32 frame clock (300 per second).
bool ticker_visible(const unsigned char *ticker, int32_t frame_clock);

}  // namespace u2mp::overlay
