// u2mp voice chat (voice.h). The codec, jitter buffer, mixer and gate come first and need nothing
// but libopus (the host test builds them alone with U2MP_VOICE_CORE_ONLY); the SDL3 streams and the
// transport side follow.
#include <utility>
#include <vector>
#include "voice.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <opus/opus.h>

namespace u2mp::voice {

// ---- codec --------------------------------------------------------------------------------------
Encoder::Encoder() {
    int error = 0;
    OpusEncoder *e = opus_encoder_create(kSampleRate, 1, OPUS_APPLICATION_VOIP, &error);
    if (error != OPUS_OK || !e) return;
    opus_encoder_ctl(e, OPUS_SET_BITRATE(24000));
    opus_encoder_ctl(e, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(e, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(e, OPUS_SET_INBAND_FEC(1));         // a lost frame can be rebuilt from the next
    opus_encoder_ctl(e, OPUS_SET_PACKET_LOSS_PERC(10));
    state_ = e;
}

Encoder::~Encoder() {
    if (state_) opus_encoder_destroy(static_cast<OpusEncoder *>(state_));
}

bool Encoder::encode(const int16_t *pcm, std::vector<uint8_t> &packet) {
    packet.clear();
    if (!state_) return false;
    packet.resize(kMaxEncoded);
    const int n = opus_encode(static_cast<OpusEncoder *>(state_), pcm, kFrameSamples, packet.data(), opus_int32(kMaxEncoded));
    if (n <= 0) {
        packet.clear();
        return false;
    }
    packet.resize(size_t(n));
    return true;
}

Decoder::Decoder() {
    int error = 0;
    OpusDecoder *d = opus_decoder_create(kSampleRate, 1, &error);
    if (error == OPUS_OK) state_ = d;
}

Decoder::~Decoder() {
    if (state_) opus_decoder_destroy(static_cast<OpusDecoder *>(state_));
}

bool Decoder::decode(const uint8_t *packet, size_t size, int16_t *pcm, bool fec) {
    if (!state_) return false;
    if (!packet) size = 0;
    const int n = opus_decode(static_cast<OpusDecoder *>(state_), packet, opus_int32(size), pcm, kFrameSamples, fec ? 1 : 0);
    if (n != kFrameSamples) {
        std::memset(pcm, 0, sizeof(int16_t) * kFrameSamples);
        return false;
    }
    return true;
}

void Decoder::reset() {
    if (state_) opus_decoder_ctl(static_cast<OpusDecoder *>(state_), OPUS_RESET_STATE);
}

// ---- jitter buffer --------------------------------------------------------------------------------
static int16_t distance(uint16_t from, uint16_t to) { return int16_t(uint16_t(to - from)); }

void JitterBuffer::clear() {
    for (Slot &s : slots_) s.used = false;
    count_ = 0;
    waited_ = 0;
    playing_ = false;
}

bool JitterBuffer::find_earliest(uint16_t &sequence) const {
    bool any = false;
    for (const Slot &s : slots_)
        if (s.used && (!any || distance(sequence, s.sequence) < 0)) {
            sequence = s.sequence;
            any = true;
        }
    return any;
}

void JitterBuffer::push(uint16_t sequence, const uint8_t *data, size_t size) {
    if (!data || !size || size > kMaxEncoded) return;
    if (playing_) {
        const int16_t ahead = distance(next_, sequence);
        if (ahead < 0) {   // already played (or skipped as lost)
            ++late;
            return;
        }
        if (ahead >= kSlots) {   // far ahead: the speaker restarted, or we fell far behind
            ++overflow;
            clear();
        }
    } else {
        for (const Slot &s : slots_)   // everything held must fit one window
            if (s.used && std::abs(int(distance(s.sequence, sequence))) >= kSlots) {
                ++overflow;
                clear();
                break;
            }
    }
    Slot &s = slots_[sequence % kSlots];
    if (s.used) {
        if (s.sequence == sequence) return;   // duplicate
        --count_;
        ++overflow;
    }
    s.used = true;
    s.sequence = sequence;
    s.data.assign(data, data + size);
    ++count_;
}

JitterBuffer::Out JitterBuffer::pop(std::vector<uint8_t> &frame, std::vector<uint8_t> *next) {
    if (!playing_) {
        if (!count_) {
            waited_ = 0;
            return Out::Idle;
        }
        // start once the target depth is held, or the first frame waited that long
        if (count_ < target_ && ++waited_ < target_) return Out::Idle;
        playing_ = true;
        waited_ = 0;
        find_earliest(next_);
    }
    // more than twice the target held (the sender's clock runs fast, or a burst): drop the oldest
    while (count_ > 2 * target_ + 2) {
        Slot &old = slots_[next_ % kSlots];
        if (old.used && old.sequence == next_) {
            old.used = false;
            --count_;
            ++overflow;
        }
        ++next_;
    }
    Slot &s = slots_[next_ % kSlots];
    if (s.used && s.sequence == next_) {
        frame.swap(s.data);
        s.used = false;
        --count_;
        ++next_;
        return Out::Frame;
    }
    if (!count_) {   // ran dry: the speaker stopped (or a long outage); rebuffer
        playing_ = false;
        return Out::Idle;
    }
    ++lost;
    ++next_;
    if (next) {
        const Slot &n = slots_[next_ % kSlots];
        if (n.used && n.sequence == next_) *next = n.data;
        else next->clear();
    }
    return Out::Lost;
}

// ---- mixer and gate --------------------------------------------------------------------------------
void mix_into(int32_t *mix, const int16_t *pcm, int samples, float gain) {
    for (int i = 0; i < samples; ++i) mix[i] += int32_t(std::lrintf(float(pcm[i]) * gain));
}

void to_pcm16(const int32_t *mix, int16_t *pcm, int samples) {
    for (int i = 0; i < samples; ++i) pcm[i] = int16_t(std::clamp<int32_t>(mix[i], -32768, 32767));
}

bool Gate::update(const int16_t *pcm, int samples) {
    double energy = 0;
    for (int i = 0; i < samples; ++i) energy += double(pcm[i]) * pcm[i];
    const float rms = samples ? float(std::sqrt(energy / samples)) : 0.0f;
    rms_ = rms;
    const bool speech = rms > std::max(floor_ * 3.0f, 300.0f);
    if (!speech) floor_ += (rms - floor_) * (rms < floor_ ? 0.2f : 0.02f);   // fall fast, rise slowly
    floor_ = std::clamp(floor_, 30.0f, 3000.0f);
    if (speech) hold_ = 15;   // 300 ms
    else if (hold_ > 0) --hold_;
    return speech || hold_ > 0;
}

// ---- talking level and mute rules (spec parity-j-voice-medals.md J1..J3) ------------------------------
float decay_talk_level(float level) { return level > 0.0f ? std::max(level - 1.0f, 0.0f) : level; }

bool is_talking(float level) { return level >= kTalkThreshold; }

VoiceUse use_voice(bool voice_on, bool gated_silent, bool muted, bool can_play) {
    const bool talking = voice_on && !gated_silent;
    return {talking, talking && !muted && can_play};
}

bool next_mute(bool muted, MuteEvent event, bool party_member) {
    switch (event) {
    case MuteEvent::Arrived: return !party_member;
    case MuteEvent::JoinedParty: return false;
    case MuteEvent::VoteOpened: return party_member && muted;
    }
    return muted;
}

// ---- mouth loudness and the talking-level table (parity-n-remaining.md N5, N6) -------------------------------------
float mouth_loudness(float rms, bool transmitting) {
    return transmitting && rms > 0.0f ? kMouthLoudnessScale * rms / 32768.0f : 0.0f;
}

int talk_table_slots(const bool *present, const float *levels, int count, uint32_t local_id, TalkSlot *out, int max) {
    int used = 0;
    for (int k = 1; k < count && used < max; ++k)
        if (present[k] && uint32_t(k) != local_id) out[used++] = TalkSlot{uint32_t(k), levels[k]};
    for (int i = used; i < max; ++i) out[i] = TalkSlot{0, 0.0f};
    return used;
}

}  // namespace u2mp::voice

#ifndef U2MP_VOICE_CORE_ONLY
// ---- runtime -----------------------------------------------------------------------------------------
#include <SDL3/SDL.h>

#include <atomic>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#include "gamenet.h"
#include "net.h"
#include "session_api.h"
#include "u2mp_internal.h"

namespace u2mp::voice {
namespace {

enum class Mode { Off, Open, PushToTalk };
struct Config {
    bool loaded = false;
    Mode mode = Mode::Open;
    SDL_Scancode ptt = SDL_SCANCODE_T;
    float volume = 1.0f;
    float start_volume = 1.0f;   // what reset_volume() goes back to
} config;

void load_config() {
    if (config.loaded) return;
    config.loaded = true;
    if (const char *m = std::getenv("UNCHARTED_U2MP_VOICE"); m && *m) {
        const std::string v(m);
        if (v == "off" || v == "0") config.mode = Mode::Off;
        else if (v == "ptt") config.mode = Mode::PushToTalk;
        else if (v != "open") log("voice: UNCHARTED_U2MP_VOICE=%s not understood (off|open|ptt), using open", m);
    }
    if (const char *k = std::getenv("UNCHARTED_U2MP_VOICE_PTT_KEY"); k && *k) {
        const SDL_Scancode code = SDL_GetScancodeFromName(k);
        if (code != SDL_SCANCODE_UNKNOWN) config.ptt = code;
        else log("voice: push-to-talk key \"%s\" unknown, using T", k);
    }
    if (const char *v = std::getenv("UNCHARTED_U2MP_VOICE_VOLUME"); v && *v)
        config.volume = std::clamp(float(std::atof(v)), 0.0f, 4.0f);
    config.start_volume = config.volume;
    static const char *const names[] = {"off", "open (voice-activity gate)", "push to talk"};
    log("voice: %s, volume %.2f", names[int(config.mode)], double(config.volume));
}

net::Transport *transport = nullptr;
SDL_AudioStream *capture = nullptr, *playback = nullptr;
bool audio_open = false, audio_failed = false, sdl_audio = false;
uint16_t send_sequence = 0;
// Per remote member (by net id), game thread. The one mute flag (PS3: the mute of that connection's voice port, J3).
bool mutes[kMaxSpeakers] = {};
bool present[kMaxSpeakers] = {};    // members seen last frame (to forget the ones who left)
bool in_party[kMaxSpeakers] = {};   // seen as a member of our party since it arrived (the party join happened)
float levels[kMaxSpeakers] = {};    // PS3 talking level (J1), mirrored into the connection records each frame

float record_level(uint32_t key) {   // gamenet::set_talk_levels: never the local player (it has no record)
    return key && key < kMaxSpeakers && key != session::local_id() ? levels[key] : 0.0f;
}
float no_level(uint32_t) { return 0.0f; }

// N6: the talking indicator readers' records (8-byte header, then kTalkSlots records of 0x220 bytes: key at +0,
// level at +0x1c8, the rest stays 0); NULL while the readers are not redirected.
constexpr uint64_t kTalkRecordStride = 0x220;
constexpr uint64_t kTalkRecordLevel = 0x1c8;
alignas(16) uint8_t talk_storage[8 + kTalkSlots * kTalkRecordStride];
uint8_t *talk_table = nullptr;

void fill_talk_table() {
    if (!talk_table) return;
    TalkSlot slots[kTalkSlots];
    talk_table_slots(present, levels, kMaxSpeakers, session::local_id(), slots, kTalkSlots);
    for (int i = 0; i < kTalkSlots; ++i) {
        uint8_t *record = talk_table + 8 + kTalkRecordStride * uint64_t(i);
        std::memcpy(record, &slots[i].key, sizeof slots[i].key);
        std::memcpy(record + kTalkRecordLevel, &slots[i].level, sizeof slots[i].level);
    }
}

// N5: M, the floor the patched 0x714d9e gives the facial talking layer (near the image, rip-relative); NULL when the
// patch is not in.
float *mouth_floor = nullptr;
bool mouth_logged = false;

// capture thread -> game thread
std::atomic<bool> ptt_down{false};
std::atomic<float> mic_loudness{0.0f};   // N5: L of the last capture frame while we transmit it, else 0
std::mutex out_mutex;
std::deque<std::vector<uint8_t>> outgoing;
std::unique_ptr<Encoder> encoder;   // capture thread only (while the stream exists)
Gate gate;

// game thread -> playback thread
struct Speaker {
    JitterBuffer jitter;
    Decoder decoder;
};
std::mutex play_mutex;
std::unique_ptr<Speaker> speakers[kMaxSpeakers];

void SDLCALL on_capture(void *, SDL_AudioStream *stream, int, int) {
    int16_t pcm[kFrameSamples];
    std::vector<uint8_t> packet;
    while (SDL_GetAudioStreamAvailable(stream) >= int(sizeof pcm)) {
        if (SDL_GetAudioStreamData(stream, pcm, sizeof pcm) != int(sizeof pcm)) break;
        const bool voiced = gate.update(pcm, kFrameSamples);   // keeps the noise floor current
        const bool talk = config.mode == Mode::PushToTalk ? ptt_down.load(std::memory_order_relaxed) : voiced;
        mic_loudness.store(mouth_loudness(gate.rms(), talk), std::memory_order_relaxed);
        if (!talk || !encoder || !encoder->encode(pcm, packet)) continue;
        std::lock_guard<std::mutex> lock(out_mutex);
        if (outgoing.size() < 25) outgoing.push_back(std::move(packet));   // 0.5 s: the game thread stalled
    }
}

void SDLCALL on_playback(void *, SDL_AudioStream *stream, int additional, int) {
    static thread_local std::vector<uint8_t> frame, next;
    while (additional > 0) {
        int32_t mix[kFrameSamples] = {};
        int16_t pcm[kFrameSamples];
        {
            std::lock_guard<std::mutex> lock(play_mutex);
            for (auto &sp : speakers) {
                if (!sp) continue;
                const JitterBuffer::Out out = sp->jitter.pop(frame, &next);
                bool ok = false;
                if (out == JitterBuffer::Out::Frame) ok = sp->decoder.decode(frame.data(), frame.size(), pcm);
                else if (out == JitterBuffer::Out::Lost)
                    ok = next.empty() ? sp->decoder.decode(nullptr, 0, pcm) : sp->decoder.decode(next.data(), next.size(), pcm, true);
                if (ok) mix_into(mix, pcm, kFrameSamples, config.volume);
            }
        }
        to_pcm16(mix, pcm, kFrameSamples);
        SDL_PutAudioStreamData(stream, pcm, sizeof pcm);
        additional -= int(sizeof pcm);
    }
}

const char *device_name(SDL_AudioStream *stream) {
    const char *n = stream ? SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(stream)) : nullptr;
    return n ? n : "?";
}

// rel32 from the instruction that ends at image offset `next` to host address `to`; false when out of reach.
bool rel32_to(uint64_t next, const void *to, int32_t &rel) {
    const int64_t delta = int64_t(uintptr_t(to)) - int64_t(uintptr_t(image.base + next));
    if (delta < INT32_MIN || delta > INT32_MAX) return false;
    rel = int32_t(delta);
    return true;
}

// N6 (parity-n-remaining.md 7.3): each talking-level reader takes the record base with one `add reg, [rip + d32]` of
// the pointer slot 0x104ae30 (the connection manager 0x1152a10, records from +8) and uses it for that search only.
// Its d32 is pointed at a slot of ours holding talk_storage's address; all four or none.
bool install_talk_table() {
    constexpr uint64_t kRecordSlot = 0x104ae30;
    struct Reader {
        uint64_t at;
        uint8_t bytes[7];
    };
    static const Reader kReaders[] = {
        {0x55b4d8, {0x48, 0x03, 0x05, 0x51, 0xf9, 0xae, 0x00}},   // overhead name tags: add rax, [rip + slot]
        {0x5b2cbf, {0x48, 0x03, 0x05, 0x6a, 0x81, 0xa9, 0x00}},   // team list voice cell (scoreboard, Mute Players)
        {0x66644b, {0x48, 0x03, 0x05, 0xde, 0x49, 0x9e, 0x00}},   // "is talking" helper
        {0x66c4d9, {0x4c, 0x03, 0x05, 0x50, 0xe9, 0x9d, 0x00}},   // talk-time counter: add r8, [rip + slot]
    };
    for (const Reader &r : kReaders) {
        int32_t disp = 0;
        std::memcpy(&disp, r.bytes + 3, 4);
        if (!image.executable(r.at, 7) || !image.matches(r.at, r.bytes, 7) ||
            int64_t(r.at) + 7 + disp != int64_t(kRecordSlot)) {
            log("voice: talking indicator reader at 0x%llx differs: the game's readers stay on the connection records "
                "(on a client only the host lights)", (unsigned long long)r.at);
            return false;
        }
    }
    unsigned char *slot = near_stub(8);
    int32_t rel[4] = {};
    for (int i = 0; i < 4; ++i)
        if (!slot || !rel32_to(kReaders[i].at + 7, slot, rel[i])) {
            log("voice: no pointer slot near the image: the talking indicator readers stay on the connection records");
            return false;
        }
    const uint64_t table = uint64_t(uintptr_t(talk_storage));
    std::memcpy(slot, &table, sizeof table);
    for (int i = 0; i < 4; ++i) std::memcpy(image.base + kReaders[i].at + 3, &rel[i], 4);
    talk_table = talk_storage;
    log("voice: talking indicators read the PC level table (4 readers)");
    return true;
}

// N5 (parity-n-remaining.md 6.3): PS4 0x714a70 (= PS3 0x51b3e8) feeds the facial talking layer's setter 0x473010
// with max(0, voice-line amplitude x 100); PS3 had the microphone loudness where PS4 has the 0. The 8 bytes at
// 0x714d9e (vxorps xmm1,xmm1,xmm1; vmaxss xmm0,xmm1,xmm0) become vmaxss xmm0,xmm0,[rip + M].
bool install_mouth() {
    struct Site {
        uint64_t at;
        const uint8_t *bytes;
        size_t size;
    };
    static const uint8_t kLineScale[] = {0xc5, 0xfa, 0x59, 0x05, 0xf2, 0xec, 0x74, 0x00};   // vmulss xmm0,[0xe63a90]
    static const uint8_t kZeroMax[] = {0xc5, 0xf0, 0x57, 0xc9, 0xc5, 0xf2, 0x5f, 0xc0};     // the 0, then the max
    static const uint8_t kSetterCall[] = {0xe8, 0x55, 0xe2, 0xd5, 0xff};                    // call 0x473010
    static const uint8_t kSetter[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                      0x53, 0x48, 0x81, 0xec, 0xa8, 0x00, 0x00, 0x00};       // its prologue
    constexpr uint64_t kPatch = 0x714d9e;
    const Site sites[] = {{0x714d96, kLineScale, sizeof kLineScale},
                          {kPatch, kZeroMax, sizeof kZeroMax},
                          {0x714db6, kSetterCall, sizeof kSetterCall},
                          {0x473010, kSetter, sizeof kSetter}};
    for (const Site &s : sites)
        if (!image.executable(s.at, s.size) || !image.matches(s.at, s.bytes, s.size)) {
            log("voice: mouth patch skipped (bytes at 0x%llx differ)", (unsigned long long)s.at);
            return false;
        }
    float *m = reinterpret_cast<float *>(near_stub(sizeof(float)));
    int32_t rel = 0;
    if (!m || !rel32_to(kPatch + 8, m, rel)) {
        log("voice: mouth patch skipped (no memory near the image)");
        return false;
    }
    *m = 0.0f;
    uint8_t patch[8] = {0xc5, 0xfa, 0x5f, 0x05};   // vmaxss xmm0, xmm0, [rip + rel]
    std::memcpy(patch + 4, &rel, 4);
    std::memcpy(image.base + kPatch, patch, sizeof patch);
    mouth_floor = m;
    log("voice: mouth follows the microphone (PS3 0x51b3e8; patch at 0x714d9e)");
    return true;
}

void open_audio() {
    audio_open = true;
    sdl_audio = SDL_InitSubSystem(SDL_INIT_AUDIO);   // reference counted: the game's audio may have it
    if (!sdl_audio) {
        log("voice: SDL audio unavailable (%s): no voice chat", SDL_GetError());
        audio_failed = true;
        return;
    }
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 1, kSampleRate};
    playback = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, on_playback, nullptr);
    if (playback) {
        SDL_ResumeAudioStreamDevice(playback);
        log("voice: playing on %s", device_name(playback));
    } else {
        log("voice: no playback stream (%s)", SDL_GetError());
    }
    encoder = std::make_unique<Encoder>();
    if (!encoder->ok()) {
        log("voice: Opus encoder unavailable: listening only");
    } else if ((capture = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, on_capture, nullptr))) {
        SDL_ResumeAudioStreamDevice(capture);
        log("voice: recording from %s", device_name(capture));
    } else {
        log("voice: no microphone (%s): listening only", SDL_GetError());
    }
    if (!playback && !capture) audio_failed = true;
}

// ---- in-match voice gating (PS3 NpNetUpdate 0x43bdd0, ShowResults::Enter 0x43ec08; spec parity-e-flow2.md 2.1) --
// Each remote talker is heard (gain 1) or not (gain 0), recomputed every frame while the match plays; outside
// play the last values stay, and the results (NET_SM 29) and the lobby side (NET_SM < 22) put everyone back to 1.
// The player's mute flag (which also holds the matchmaking stranger mute, J3) is separate and still wins.
std::atomic<bool> silenced[kMaxSpeakers];   // zero (static) = heard
bool gating_on = false;
int32_t gating_sm = 0;

constexpr uint64_t kNetSm = 0x14ce97c;
constexpr uint64_t kNetInfo = 0x104f1c8;
constexpr uint64_t kVoiceOff = kNetInfo + 0x2c8;   // the "voice off" option (PS3 netInfo +0x2b8)
constexpr uint64_t kNgm = 0x113b760;
constexpr uint64_t kNgmState = kNgm + 0x1e80;      // 2 = playing
constexpr uint64_t kTrackers = kNgm + 0x10;
constexpr uint64_t kTrackerStride = 0x2c0;
constexpr uint64_t kPlayerPosition = 0x665f70;     // guest (vec4 *out, tracker *), as visibility.cpp
constexpr uint64_t kLookup = 0xa15e70;
constexpr uint32_t kNetGamesId = 0x3b9a067d;
constexpr float kEnemyRange = 12.0f;
constexpr int32_t kElimination = 10;

template <typename T> T peek(const uint8_t *p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
const uint8_t *game(uint64_t offset) { return image.base + offset; }

int32_t current_game_type() {
    const int16_t index = peek<int16_t>(game(kNgm + 0x1ca0));
    const auto *table = reinterpret_cast<const uint8_t *(U2MP_GUEST *)(uint32_t)>(image.base + kLookup)(kNetGamesId);
    if (!table || index < 0 || index >= peek<int32_t>(table)) return -1;
    const uint8_t *records = peek<const uint8_t *>(table + 8);
    return records ? peek<int32_t>(records + 0x90 * uint64_t(index) + 0xc) : -1;
}

// The tracker's character exists and is alive: its handle (+0x10 slot, +0x18 id) resolves and the character's
// alive test (vtable slot 74) says so.
bool character_alive(const uint8_t *tracker) {
    const uint8_t *slot = peek<const uint8_t *>(tracker + 0x10);
    const uint32_t id = peek<uint32_t>(tracker + 0x18);
    if (!slot || !id) return false;
    uint8_t *character = peek<uint8_t *>(slot);
    if (!character || peek<uint32_t>(character + 0x44) != id) return false;
    const uint8_t *vtable = peek<const uint8_t *>(character);
    const uint64_t fn = vtable ? peek<uint64_t>(vtable + 74 * 8) : 0;
    const uint64_t base = uint64_t(uintptr_t(image.base));
    if (fn < base || !image.executable(fn - base, 1)) return false;
    return reinterpret_cast<uint8_t(U2MP_GUEST *)(void *)>(fn)(character) != 0;
}

void set_audible(uint8_t id, bool on) {
    if (!silenced[id].exchange(!on) && !on) {   // just silenced: drop what is already buffered
        std::lock_guard<std::mutex> lock(play_mutex);
        if (speakers[id]) speakers[id]->jitter.clear();
    }
}

void all_audible(const char *why) {
    for (uint8_t id = 0; id < kMaxSpeakers; ++id) set_audible(id, true);
    if (gating_on) log("voice: every player heard again (%s)", why);
    gating_on = false;
}

void update_gating() {
    if (!image.base) return;
    const int32_t sm = peek<int32_t>(game(kNetSm));
    if (sm != gating_sm) {
        if (sm == 29) all_audible("results");   // PS3 ShowResults::Enter: every remote voice at 1.0
        gating_sm = sm;
    }
    if (sm < 22) {
        if (gating_on) all_audible("lobby");
        return;
    }
    const uint8_t *local = peek<const uint8_t *>(game(kNgm));
    if (peek<int32_t>(game(kNgmState)) != 2 || !local) return;   // not playing: nothing changes
    const int32_t type = current_game_type();
    const bool voice_off = peek<uint8_t>(game(kVoiceOff)) != 0;
    if (!gating_on) log("voice: in-match gating on (game type %d%s)", int(type), voice_off ? ", voice off" : "");
    gating_on = true;
    const bool local_alive = character_alive(local);
    const int32_t local_team = peek<int32_t>(local + 0x134);
    const uint32_t local_id = peek<uint32_t>(local + 0x120);
    alignas(16) float here[4] = {};
    if (local_alive) reinterpret_cast<uint64_t(U2MP_GUEST *)(float *, const uint8_t *)>(image.base + kPlayerPosition)(here, local);
    for (int i = 0; i < 10; ++i) {
        const uint8_t *t = game(kTrackers + uint64_t(i) * kTrackerStride);
        const uint32_t id = peek<uint32_t>(t + 0x120);
        if (t == local || id == local_id || !id || id >= kMaxSpeakers || t[0x20] == 0 || t[0x26c] != 0) continue;
        bool on;
        if (voice_off) {
            on = false;
        } else {
            const bool alive = character_alive(t);
            const bool enemy = peek<int32_t>(t + 0x134) != local_team;
            bool near = false;
            if (enemy && alive && local_alive) {
                alignas(16) float there[4] = {};
                reinterpret_cast<uint64_t(U2MP_GUEST *)(float *, const uint8_t *)>(image.base + kPlayerPosition)(there, t);
                const float dx = there[0] - here[0], dy = there[1] - here[1], dz = there[2] - here[2];
                near = dx * dx + dy * dy + dz * dz < kEnemyRange * kEnemyRange;
            }
            if (type == kElimination)   // the dead talk among themselves; the living hear the living
                on = local_alive ? alive && (!enemy || near) : !alive;
            else                        // teammates always; enemies within 12 m while both characters exist
                on = !enemy || near;
        }
        set_audible(uint8_t(id), on);
    }
}

// A member left (or the slot changed hands): forget its stream, mute and talking level.
void forget(uint8_t id) {
    mutes[id] = false;
    in_party[id] = false;
    levels[id] = 0.0f;
    silenced[id] = false;
    std::lock_guard<std::mutex> lock(play_mutex);
    speakers[id].reset();
}

void apply_mute(uint32_t id, bool on) {
    mutes[id] = on;
    if (on) {   // stop what is already buffered
        std::lock_guard<std::mutex> lock(play_mutex);
        if (speakers[id]) speakers[id]->jitter.clear();
    }
}

// PS3 parity J3: the connection reset mutes a new member unless it is in our party; the party join unmutes.
void track_member(uint8_t id) {
    const bool party = session::party_member(id);
    if (!present[id]) {
        in_party[id] = party;
        apply_mute(id, next_mute(false, MuteEvent::Arrived, party));
        if (mutes[id]) log("voice: net id %u connected outside our party: muted until the vote opens", unsigned(id));
    } else if (party && !in_party[id]) {
        in_party[id] = true;
        if (mutes[id]) log("voice: net id %u joined our party: unmuted", unsigned(id));
        apply_mute(id, next_mute(mutes[id], MuteEvent::JoinedParty, true));
    }
}

}  // namespace

bool enabled() {
    load_config();
    return config.mode != Mode::Off;
}

void set_transport(net::Transport *t) {
    if (t != transport) stop();
    transport = t;
}

void stop() {
    if (capture) SDL_DestroyAudioStream(capture);   // no callback runs after this returns
    if (playback) SDL_DestroyAudioStream(playback);
    capture = playback = nullptr;
    encoder.reset();
    if (sdl_audio) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    if (audio_open) log("voice: closed");
    sdl_audio = audio_open = audio_failed = false;
    {
        std::lock_guard<std::mutex> lock(out_mutex);
        outgoing.clear();
    }
    for (uint8_t id = 0; id < kMaxSpeakers; ++id) {
        present[id] = false;
        speakers[id].reset();   // no playback callback is left to race with
        mutes[id] = false;
        in_party[id] = false;
        levels[id] = 0.0f;
        silenced[id] = false;
    }
    gamenet::set_talk_levels(no_level);
    fill_talk_table();   // nobody present: every slot key 0, level 0
    mic_loudness.store(0.0f, std::memory_order_relaxed);
    if (mouth_floor) *mouth_floor = 0.0f;
    gating_on = false;
    gate = Gate{};
}

void install(int &installed) {
    if (install_talk_table()) ++installed;
    if (install_mouth()) ++installed;
}

void frame() {
    if (!transport) return;
    // members: forget the ones who left, mute the new ones outside our party (J3); is there anyone to talk to?
    bool others = false;
    const uint32_t me = session::local_id();
    for (uint8_t id = 1; id < kMaxSpeakers; ++id) {
        std::string name;
        const bool here = session::member_name(id, name);
        if (present[id] && !here) forget(id);
        if (here && id != me) track_member(id);
        present[id] = here;
        others = others || (here && id != me);
    }
    // talking levels (J1): 1.0 lower each game frame, then into the connection records and the readers' PC table (N6:
    // every present remote member, also the other clients of a star)
    for (float &level : levels) level = decay_talk_level(level);
    gamenet::set_talk_levels(record_level);
    fill_talk_table();
    if (!enabled()) return;
    if (others && !audio_open) open_audio();
    // N5: the mouth floor M, while the capture stream is open (0 when we do not transmit)
    if (mouth_floor) {
        const float m = capture ? mic_loudness.load(std::memory_order_relaxed) : 0.0f;
        *mouth_floor = m;
        if (m > 0.0f && !mouth_logged) {
            mouth_logged = true;
            log("voice: the microphone moves the own character's mouth (first loudness %.2f)", double(m));
        }
    }
    update_gating();
    if (config.mode == Mode::PushToTalk) {
        const bool *keys = SDL_GetKeyboardState(nullptr);
        ptt_down.store(keys && keys[config.ptt], std::memory_order_relaxed);
    }
    std::deque<std::vector<uint8_t>> batch;
    {
        std::lock_guard<std::mutex> lock(out_mutex);
        batch.swap(outgoing);
    }
    if (!others || !me || me >= kMaxSpeakers) return;
    std::vector<uint8_t> message;
    for (const auto &packet : batch) {
        message.assign({gamenet::kVoice, uint8_t(me), uint8_t(send_sequence), uint8_t(send_sequence >> 8)});
        message.insert(message.end(), packet.begin(), packet.end());
        ++send_sequence;
        if (transport->is_host()) transport->broadcast(false, message.data(), message.size());
        else transport->send(0, false, message.data(), message.size());
    }
}

void receive(uint8_t slot, const uint8_t *data, size_t size) {
    if (!transport || size < 4 || size - 3 > kMaxEncoded) return;
    uint8_t speaker = data[0];
    if (transport->is_host()) {
        if (slot == 0 || slot > net::kMaxPeers) return;
        speaker = uint8_t(slot + 1);   // whoever sent it to us is the speaker
        // relay to the other clients (also when our own voice is off: they still hear each other)
        std::vector<uint8_t> message{gamenet::kVoice, speaker};
        message.insert(message.end(), data + 1, data + size);
        for (uint8_t s = 1; s <= net::kMaxPeers; ++s)
            if (s != slot) transport->send(s, false, message.data(), message.size());
    } else if (slot != 0) {
        return;
    }
    if (!speaker || speaker >= kMaxSpeakers || speaker == session::local_id()) return;
    // J2: the talking level rises also for muted players and strangers (mute only silences); not with our voice off
    // and not for a speaker the in-match gating silences (PS3 set that voice's volume to 0 and did not send to it).
    const VoiceUse use = use_voice(enabled(), silenced[speaker], mutes[speaker], playback != nullptr);
    if (use.talking) levels[speaker] = kTalkOnVoice;
    if (!use.played) return;
    const uint16_t sequence = uint16_t(data[1] | data[2] << 8);
    std::lock_guard<std::mutex> lock(play_mutex);
    if (!speakers[speaker]) speakers[speaker] = std::make_unique<Speaker>();
    speakers[speaker]->jitter.push(sequence, data + 3, size - 3);
}

bool muted(uint32_t net_id) { return net_id < kMaxSpeakers && net_id != session::local_id() && mutes[net_id]; }

bool talking(uint32_t net_id) {
    return net_id && net_id < kMaxSpeakers && net_id != session::local_id() && is_talking(levels[net_id]);
}

bool toggle_mute(uint32_t net_id) {
    if (!net_id || net_id >= kMaxSpeakers || net_id == session::local_id()) return false;
    apply_mute(net_id, !mutes[net_id]);
    log("voice: net id %u %s", net_id, mutes[net_id] ? "muted" : "unmuted");
    return mutes[net_id];
}

void unmute_strangers() {
    int unmuted = 0;
    const uint32_t me = session::local_id();
    for (uint8_t id = 1; id < kMaxSpeakers; ++id) {
        if (!present[id] || id == me) continue;
        const bool on = next_mute(mutes[id], MuteEvent::VoteOpened, session::party_member(id));
        if (on == mutes[id]) continue;
        apply_mute(id, on);
        ++unmuted;
    }
    log("voice: the vote opens: players outside our party are heard now (%d unmuted)", unmuted);
}

void set_muted(uint32_t net_id, bool on) {
    if (!net_id || net_id >= kMaxSpeakers || mutes[net_id] == on) return;
    toggle_mute(net_id);
}

// PS3 parity H7 (0x4d6fa0 / 0x4d6d10 / 0x4d6ab0) over the caller's "others" (remote members with a record).
bool all_muted(const std::vector<uint32_t> &others) {
    for (const uint32_t id : others)
        if (!muted(id)) return false;
    return true;
}

bool none_muted(const std::vector<uint32_t> &others) {
    for (const uint32_t id : others)
        if (muted(id)) return false;
    return true;
}

void set_all_muted(const std::vector<uint32_t> &others, bool on) {
    for (const uint32_t id : others) set_muted(id, on);
}

float volume() {
    load_config();
    return config.volume;
}
void set_volume(float v) {
    load_config();
    config.volume = std::clamp(v, 0.0f, 4.0f);
    log("voice: volume %.2f (menu)", double(config.volume));
}
void reset_volume() {
    load_config();
    if (config.volume != config.start_volume) log("voice: volume back to %.2f (online multiplayer entered)", double(config.start_volume));
    config.volume = config.start_volume;
}

}  // namespace u2mp::voice
#endif
