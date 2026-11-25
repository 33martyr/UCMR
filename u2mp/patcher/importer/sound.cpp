// importer/sound.cpp: the sounds of the Uncharted 2 import.
//
// XVAG streams (music, speech, effects): PS3 XVAG (big-endian header, PS-ADPCM codec 6) -> the PS4 XVAG the game reads.
// No re-encoding; only the container changes (the four PS-ADPCM music tracks retail PS4 ships, 146/148/149/150_
// JungleExplore_*, are the reference pairs):
//   header  'XVAG', u32 header size, version, 0, 0, flags, 20 zero bytes (PS3 01 00 00 60, PS4 02 00 00 62; PS4 words
//           little endian)
//   chunks  tag, u32 size, data. fmat: PS3 7 words (channels, codec, samples, samples, 1, rate, data size); PS4 the same
//           7 words with word 4 = the interleave (16-byte frames per channel and block) and then 1, channels. cpan: the
//           words byte-swapped except word 2 (a float PS3 already stores little endian). 0000 ends the header.
//   data    PS3 interleaves the channels frame by frame; PS4 stores blocks of <interleave> frames per channel, the
//           last block zero-padded (one channel: padded to a whole block).
// Sound banks (SCREAM SBlk v4 / v5 -> v13): the Uncharted 3 converter (u3mp/patcher/tools/fixes/sound-banks); every
// sample keeps its own codec (no ffmpeg since converter version 2).
#include "u2import.hpp"
#include "u3fix.hpp"   // u3::sound_bank_convert, u3::find_ffmpeg
#include "u2conv.hpp"

#include <map>

namespace u2imp {
namespace {

// frames per channel and block: XVAG_INTERLEAVE (u2import.hpp; retail 148/149_JungleExplore)

uint32_t be(const Bytes &d, size_t o) {
    if (o + 4 > d.size()) throw std::invalid_argument("XVAG: truncated");
    return uint32_t(d[o]) << 24 | uint32_t(d[o + 1]) << 16 | uint32_t(d[o + 2]) << 8 | d[o + 3];
}
void le(Bytes &o, uint32_t v) {
    for (int k = 0; k < 4; k++) o.push_back(uint8_t(v >> (8 * k)));
}

}  // namespace

Bytes xvag_convert(const Bytes &d, uint32_t interleave) {
    if (d.size() < 4 || std::string(d.begin(), d.begin() + 4) != "XVAG") throw std::invalid_argument("ValueError: not an XVAG file");
    const uint32_t hs = be(d, 4);
    if (hs > 0x1000 || d[4] || d[5]) throw std::invalid_argument("ValueError: not a big-endian (PS3) XVAG");
    struct Chunk {
        std::string tag;
        Bytes data;
    };
    std::vector<Chunk> chunks;
    for (size_t o = 0x20; o + 8 <= hs;) {
        const uint32_t size = be(d, o + 4);
        const size_t a = std::min(d.size(), o + 8), b = std::min(d.size(), o + 8 + size);
        chunks.push_back({std::string(d.begin() + long(o), d.begin() + long(o + 4)), Bytes(d.begin() + long(a), d.begin() + long(b))});
        o += 8 + size;
    }
    bool ok = !chunks.empty() && chunks.front().tag == "fmat" && chunks.back().tag == "0000";
    for (const Chunk &c : chunks) ok = ok && (c.tag == "fmat" || c.tag == "cpan" || c.tag == "0000");
    if (!ok) throw std::invalid_argument("ValueError: unexpected XVAG chunks");
    const Bytes &f = chunks[0].data;
    if (f.size() < 28) throw std::invalid_argument("XVAG: short fmat");
    const uint32_t ch = be(f, 0), codec = be(f, 4), ns = be(f, 8), ns2 = be(f, 12), rate = be(f, 20), dsize = be(f, 24);
    if (codec != 6) throw std::invalid_argument(fmt("ValueError: XVAG codec %u (only PS-ADPCM, codec 6, is converted)", codec));
    if (!ch || size_t(hs) + dsize > d.size() || dsize % (16 * ch))
        throw std::invalid_argument(fmt("ValueError: XVAG data size %u does not hold whole frames for %u channels", dsize, ch));
    const uint8_t *data = d.data() + hs;
    const uint32_t nfr = dsize / 16 / ch;   // frames per channel
    const uint32_t il = ch > 1 ? interleave : std::max(1u, std::min(interleave, nfr));
    Bytes body_data;
    if (ch > 1) {
        const uint32_t nblk = (nfr + il - 1) / il;
        body_data.reserve(size_t(nblk) * ch * il * 16);
        for (uint32_t b = 0; b < nblk; b++)
            for (uint32_t c = 0; c < ch; c++)
                for (uint32_t k = 0; k < il; k++) {
                    const uint32_t fr = b * il + k;   // the channel's frame
                    if (fr < nfr) body_data.insert(body_data.end(), data + 16 * (size_t(fr) * ch + c), data + 16 * (size_t(fr) * ch + c) + 16);
                    else body_data.insert(body_data.end(), 16, 0);
                }
    } else {
        body_data.assign(data, data + dsize);
        const size_t blk = 16 * size_t(il);
        body_data.resize(body_data.size() + (blk - body_data.size() % blk) % blk, 0);
    }
    Bytes body;
    for (const Chunk &c : chunks) {
        Bytes c2;
        if (c.tag == "fmat") {
            for (uint32_t v : {ch, codec, ns, ns2, il, rate, dsize, 1u, ch}) le(c2, v);
        } else if (c.tag == "cpan") {
            const size_t n = c.data.size() / 4;
            for (size_t k = 0; k < n; k++) le(c2, be(c.data, 4 * k));
            if (n > 2)
                for (int k = 0; k < 4; k++) c2[8 + k] = c.data[8 + k];   // the float is in PS4 byte order already
            c2.insert(c2.end(), c.data.begin() + long(4 * n), c.data.end());
        } else
            c2 = c.data;
        body.insert(body.end(), c.tag.begin(), c.tag.end());
        le(body, uint32_t(c2.size()));
        body.insert(body.end(), c2.begin(), c2.end());
    }
    Bytes out = {'X', 'V', 'A', 'G'};
    le(out, uint32_t(0x20 + body.size()));
    for (uint8_t b : {2, 0, 0, 0x62}) out.push_back(b);
    out.insert(out.end(), 20, 0);
    out.insert(out.end(), body.begin(), body.end());
    out.insert(out.end(), body_data.begin(), body_data.end());
    return out;
}

// --ffmpeg, the package's u3mp/bin/ffmpeg.exe, then RECIPE_FFMPEG, msys2 and PATH (as the U3 recipe looks)
std::string find_ffmpeg(const std::string &arg) {
    if (!arg.empty() && fs::is_regular_file(P(arg))) return S(fs::absolute(P(arg)));
    const fs::path bin = launcher_root() / "u3mp" / "bin" / "ffmpeg.exe";
    std::error_code ec;
    if (fs::is_regular_file(bin, ec)) return S(fs::absolute(bin));
    return u3::find_ffmpeg("");
}

// dc1/ps4/audio.bin: the sound records of the game's own PS4 module, with the falloff of the multiplayer characters'
// foley as PS3 1.10 has it. Every sound is one symbol whose record (type word 0xee0bff2f before it) starts with the
// symbol's id:
//   PS3 (big endian, 0x5c bytes):    +0 u32 id, +4 u32 name, +8 flags, +0xc group, half floats from +0x20
//   PS4 (little endian, 0x68 bytes): +0 u32 id, +8 u64 name, +0x10 flags, +0x14 group, the same half floats from +0x28
// (+0 gain in dB, +4 falloff start, +8 falloff end, ...). The PS4 records of the "mlb-" and "moc-" sounds (the foley of
// the multiplayer characters) end their falloff at 8 m (15 m for 32 of them); PS3 ends it at 15 m (30 m). Every other
// half float of every record the two modules share is the same. The rule: a PS4 record named mlb-* or moc-* whose sound
// PS3 has under the same id and name takes PS3's falloff end. Nothing else changes (the module's size, its other
// records and fields, and the names: PS3 spells its records the same way as PS4).
const char *AUDIO_VERSION = "u2audio1";

namespace {

constexpr uint32_t kSoundType = 0xee0bff2f;
constexpr size_t kPs3SoundSize = 0x5c, kPs4SoundSize = 0x68;
constexpr size_t kPs3FalloffEnd = 0x28, kPs4FalloffEnd = 0x30;

uint32_t audio_u32(const Bytes &d, size_t o, bool big) {
    if (o + 4 > d.size()) throw std::invalid_argument("audio.bin: truncated");
    return big ? uint32_t(d[o]) << 24 | uint32_t(d[o + 1]) << 16 | uint32_t(d[o + 2]) << 8 | d[o + 3]
               : uint32_t(d[o]) | uint32_t(d[o + 1]) << 8 | uint32_t(d[o + 2]) << 16 | uint32_t(d[o + 3]) << 24;
}
uint64_t audio_u64(const Bytes &d, size_t o) { return uint64_t(audio_u32(d, o, false)) | uint64_t(audio_u32(d, o + 4, false)) << 32; }
std::string audio_name(const Bytes &d, uint64_t o) {
    std::string s;
    while (o < d.size() && d[size_t(o)] && s.size() < 256) s += char(d[size_t(o++)]);
    return s;
}

struct SoundRecord {
    size_t at;
    std::string name;
};

// id -> record of every sound of a U2 DC module (PS3 'DC00' big endian or PS4 '00CD')
std::map<uint32_t, SoundRecord> sound_records(const Bytes &d, bool ps3) {
    if (d.size() < 0x20 || std::string(d.begin(), d.begin() + 4) != (ps3 ? "DC00" : "00CD"))
        throw std::invalid_argument(ps3 ? "audio.bin: not a PS3 DC module" : "audio.bin: not a PS4 DC module");
    const uint64_t root = ps3 ? audio_u32(d, 0x10, true) : audio_u64(d, 0x10);
    const uint32_t n = audio_u32(d, size_t(root + 4), ps3);
    const size_t es = ps3 ? 8 : 16, size = ps3 ? kPs3SoundSize : kPs4SoundSize;
    std::map<uint32_t, SoundRecord> out;
    for (uint32_t i = 0; i < n; i++) {
        const size_t o = size_t(root + 8 + es * i);
        const uint32_t id = audio_u32(d, o, ps3);
        const uint64_t at = ps3 ? audio_u32(d, o + 4, true) : audio_u64(d, o + 8);
        if (at < 4 || at + size > d.size() || audio_u32(d, size_t(at - 4), ps3) != kSoundType || audio_u32(d, size_t(at), ps3) != id)
            continue;
        const uint64_t name = ps3 ? audio_u32(d, size_t(at + 4), true) : audio_u64(d, size_t(at + 8));
        out[id] = {size_t(at), audio_name(d, name)};
    }
    return out;
}

}  // namespace

Bytes audio_records_convert(const Bytes &ps4, const Bytes &ps3, std::string &note) {
    const auto r4 = sound_records(ps4, false), r3 = sound_records(ps3, true);
    Bytes out = ps4;
    size_t foley = 0, changed = 0;
    for (const auto &[id, a] : r4) {
        if (a.name.rfind("mlb-", 0) != 0 && a.name.rfind("moc-", 0) != 0) continue;
        const auto it = r3.find(id);
        if (it == r3.end() || it->second.name != a.name) continue;
        ++foley;
        const size_t b = it->second.at + kPs3FalloffEnd;
        const uint8_t lo = ps3[b + 1], hi = ps3[b];   // the PS3 half float, big endian
        uint8_t *p = out.data() + a.at + kPs4FalloffEnd;
        if (p[0] == lo && p[1] == hi) continue;
        p[0] = lo;
        p[1] = hi;
        ++changed;
    }
    note = fmt("%zu sounds (PS3 %zu); %zu multiplayer foley records, %zu take the PS3 falloff end", r4.size(), r3.size(), foley,
               changed);
    return out;
}

Bytes bank_convert(const Bytes &ps3, const std::string &ffmpeg, std::string &note) {
    try {
        return u3::sound_bank_convert(ps3, ffmpeg, launcher_root() / "u3mp" / "patcher" / "tools" / "fixes" / "sound-banks" / "adsr_env.json", note);
    } catch (const std::invalid_argument &e) {
        throw Unsupported(e.what());
    }
}

}  // namespace u2imp
