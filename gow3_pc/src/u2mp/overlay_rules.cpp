// u2mp: the pure overlay and ticker rules (overlay_rules.h).
//
// Reimplemented from the behaviour descriptions u2mp/docs/spec/parity-m-features.md (M1, M2),
// u2mp/docs/spec/parity-j-voice-medals.md (J4, J5) and u2mp/docs/spec/parity-ticker-respawn.md (1.2 - 1.6).
#include "overlay_rules.h"

#include <cmath>
#include <cstring>

namespace u2mp::overlay {
namespace {

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

float f32_at(const unsigned char *p, uint32_t off) {
    float v;
    std::memcpy(&v, p + off, sizeof v);
    return v;
}
uint32_t entry_at(int k) { return kTickerEntry + uint32_t(k) * kTickerStride; }
bool in_use(const unsigned char *ticker, int k) { return ticker[entry_at(k) + kTickerTextOff] != 0; }
float start_of(const unsigned char *ticker, int k) { return f32_at(ticker, entry_at(k)); }
float width_of(const unsigned char *ticker, int k) { return f32_at(ticker, entry_at(k) + 4); }

}  // namespace

float fade_step(float fade, bool shown, float speed, float dt) {
    return clamp01(shown ? fade + speed * dt : fade - speed * dt);
}

int top_entry(const float (&fade)[kEntries]) {
    for (int k = kEntries - 1; k >= 0; --k)
        if (fade[k] > 0.0f) return k;
    return -1;
}

float slide_ease(float fade, bool shown) {
    if (shown) return 1.0f - (1.0f - fade) * (1.0f - fade);
    return fade * fade;
}

float slide_y(float fade, bool shown) {
    const float rest = kSlide * (1.0f - slide_ease(fade, shown));
    return shown ? -rest : rest;
}

int state_entry(int32_t ngm_state, int32_t round_substate, bool cinema) {
    if (ngm_state == 3) return 2;
    if (ngm_state == 4) return 3;
    if (ngm_state == 5 && !cinema) return round_substate == 3 ? 3 : 0;
    return -1;
}

Edge auto_edge(bool &last, bool now) {
    if (now == last) return Edge::None;
    last = now;
    return now ? Edge::Opened : Edge::Closed;
}

bool voice_cell_muted(int32_t row_kind, uint32_t list_page, uint32_t tracker_index, uint32_t column_id, bool muted) {
    return row_kind == kPlayerRow && list_page != kPreviousGameList && tracker_index < kTrackers &&
           column_id == kVoiceColumn && muted;
}

int32_t mute_action_team(uint32_t action, int32_t own_team, bool &known) {
    known = action == kActionOwnTeamList || action == kActionOtherTeamList;
    return action == kActionOwnTeamList ? own_team : 1 - own_team;
}

bool mute_action_previous_game(int32_t ngm_state) { return ngm_state != 1 && ngm_state != 2; }

int team_row_entry(const TeamEntry *entries, uint32_t count, int32_t team, int32_t row) {
    for (uint32_t i = 0; i < count; ++i)
        if (int32_t(entries[i].team) == team && int64_t(entries[i].position) == int64_t(row) + 1) return int(i);
    return -1;
}

float hint_step(float h, bool on, float dt) { return clamp01(on ? h + kHintIn * dt : h - kHintOut * dt); }

float ticker_next_start(const unsigned char *ticker) {
    float start = f32_at(ticker, kTickerWidth) + kTickerEdgeGap;
    for (int k = 0; k < kTickerSlots; ++k) {
        if (!in_use(ticker, k)) continue;
        const float behind = start_of(ticker, k) + width_of(ticker, k) + kTickerQueueGap;
        if (behind > start) start = behind;
    }
    return start;
}

int ticker_free_entry(const unsigned char *ticker) {
    for (int k = 0; k < kTickerSlots; ++k)
        if (!in_use(ticker, k)) return k;
    return -1;
}

bool ticker_finished(const unsigned char *ticker) {
    const float width = f32_at(ticker, kTickerWidth);
    for (int k = 0; k < kTickerSlots; ++k)
        if (in_use(ticker, k) && start_of(ticker, k) + kTickerQueueGap + width_of(ticker, k) > width) return false;
    return true;
}

bool ticker_visible(const unsigned char *ticker, int32_t frame_clock) {
    const float now = float(frame_clock) * (1.0f / 300.0f);
    return std::fabs(double(now) - double(f32_at(ticker, kTickerDrawn))) < kTickerVisibleSeconds;
}

}  // namespace u2mp::overlay
