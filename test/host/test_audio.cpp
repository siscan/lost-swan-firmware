// Audio: WAV parsing, the cue table, gain and quiet hours (spec 9).
//
// The parser reads a file anyone can upload from the Settings page, so it is
// tested the way the ring upload is - the malformed cases first, because a
// header that lies about its length would stream whatever follows it in the
// filesystem out of the speaker.
#include <cstring>
#include <string>
#include <vector>

#include "audio/wav.h"
#include "check.h"

using namespace swan;
using namespace swan::audio;

namespace {

void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 24));
}
void put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
}
void tag(std::vector<uint8_t>& v, const char* t) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(t[i]));
}

struct WavOpts {
    uint16_t format = 1;
    uint16_t channels = 1;
    uint32_t rate = 22050;
    uint16_t bits = 16;
    uint32_t samples = 100;
    bool extra_chunk = false;    // a LIST between fmt and data
    uint32_t lie_data_size = 0;  // announce more than is present
};

std::vector<uint8_t> make_wav(const WavOpts& o = {}) {
    std::vector<uint8_t> body;
    tag(body, "fmt ");
    put32(body, 16);
    put16(body, o.format);
    put16(body, o.channels);
    put32(body, o.rate);
    put32(body, o.rate * o.channels * (o.bits / 8));
    put16(body, static_cast<uint16_t>(o.channels * (o.bits / 8)));
    put16(body, o.bits);
    if (o.extra_chunk) {
        tag(body, "LIST");
        put32(body, 6);
        for (int i = 0; i < 6; ++i) body.push_back('x');
    }
    const uint32_t data_bytes = o.samples * (o.bits / 8) * o.channels;
    tag(body, "data");
    put32(body, o.lie_data_size != 0 ? o.lie_data_size : data_bytes);
    for (uint32_t i = 0; i < data_bytes; ++i) body.push_back(static_cast<uint8_t>(i));

    std::vector<uint8_t> f;
    tag(f, "RIFF");
    put32(f, static_cast<uint32_t>(body.size() + 4));
    tag(f, "WAVE");
    f.insert(f.end(), body.begin(), body.end());
    return f;
}

void test_a_good_file() {
    const std::vector<uint8_t> f = make_wav();
    const WavInfo w = wav_parse(f.data(), f.size());
    CHECK(w.ok);
    CHECK_EQ(w.sample_rate, 22050);
    CHECK_EQ(w.channels, 1);
    CHECK_EQ(w.bits, 16);
    CHECK_EQ(w.data_bytes, 200);
    CHECK(w.data_offset > 0 && w.data_offset + w.data_bytes <= f.size());
    // 16 kHz is explicitly acceptable (spec 9).
    WavOpts o;
    o.rate = 16000;
    const std::vector<uint8_t> g = make_wav(o);
    CHECK(wav_parse(g.data(), g.size()).ok);
}

// A chunk table, not a fixed 44-byte offset: plenty of encoders put LIST/INFO
// between fmt and data, and a fixed offset reads metadata as audio.
void test_chunks_are_walked() {
    WavOpts o;
    o.extra_chunk = true;
    const std::vector<uint8_t> f = make_wav(o);
    const WavInfo w = wav_parse(f.data(), f.size());
    CHECK(w.ok);
    CHECK_EQ(w.data_bytes, 200);
    // The samples really are where it says they are.
    CHECK_EQ(f[w.data_offset], 0);
    CHECK_EQ(f[w.data_offset + 1], 1);
}

void test_rejects_what_it_should() {
    const std::vector<uint8_t> good = make_wav();

    // Truncated at every length: an upload that dies partway must not parse.
    for (std::size_t n = 0; n < good.size(); n += 7) {
        const WavInfo w = wav_parse(good.data(), n);
        if (w.ok) {
            // Only acceptable if the header AND the announced data are inside.
            CHECK(w.data_offset + w.data_bytes <= n);
        }
    }
    CHECK(!wav_parse(nullptr, 0).ok);

    // Not a WAV at all - the ring.json somebody picked by mistake.
    std::vector<uint8_t> json(200, '{');
    CHECK(!wav_parse(json.data(), json.size()).ok);

    WavOpts o;
    o.channels = 2;
    std::vector<uint8_t> f = make_wav(o);
    CHECK(!wav_parse(f.data(), f.size()).ok);

    o = WavOpts{};
    o.bits = 8;
    f = make_wav(o);
    CHECK(!wav_parse(f.data(), f.size()).ok);

    o = WavOpts{};
    o.format = 3;          // IEEE float, not PCM
    f = make_wav(o);
    CHECK(!wav_parse(f.data(), f.size()).ok);

    o = WavOpts{};
    o.rate = 48000;        // outside the spec range
    f = make_wav(o);
    CHECK(!wav_parse(f.data(), f.size()).ok);

    o = WavOpts{};
    o.samples = 0;
    f = make_wav(o);
    CHECK(!wav_parse(f.data(), f.size()).ok);
}

// THE dangerous case: a header that announces more data than the file holds.
// Streaming the announced length would play whatever follows in the
// filesystem out of the speaker.
void test_a_lying_header_is_clamped() {
    WavOpts o;
    o.samples = 100;
    o.lie_data_size = 100000;
    const std::vector<uint8_t> f = make_wav(o);
    const WavInfo w = wav_parse(f.data(), f.size());
    CHECK(w.ok);
    CHECK(w.data_offset + w.data_bytes <= f.size());
    CHECK_EQ(w.data_bytes, 200);
}

void test_cue_table() {
    for (std::size_t i = 0; i < CUE_COUNT; ++i) {
        const CueId c = static_cast<CueId>(i);
        CueId back{};
        CHECK(cue_id_from_name(cue_id_name(c), back));
        CHECK(back == c);
        // One directory, one name per cue, so an upload knows what it replaces.
        const std::string p = cue_path(c);
        CHECK(p.rfind("/fs/audio/", 0) == 0);
        CHECK(p.size() > 14 && p.substr(p.size() - 4) == ".wav");
    }
    CueId junk{};
    CHECK(!cue_id_from_name("", junk));
    CHECK(!cue_id_from_name("alarm", junk));

    // The alarm loops, bounded. Unbounded would mean a display in an empty
    // house screaming until somebody comes home.
    const CuePolicy fail = cue_policy(CueId::SystemFailure, 60);
    CHECK(fail.loop);
    CHECK_EQ(fail.max_loop_s, 60);
    CHECK(fail.interrupts);
    // The warnings do not loop.
    CHECK(!cue_policy(CueId::Warn4Min, 60).loop);
    CHECK(!cue_policy(CueId::Warn1Min, 60).loop);
    // A UI click must never cut off the four-minute warning.
    CHECK(!cue_policy(CueId::UiExecute, 60).interrupts);
    CHECK(!cue_policy(CueId::UiReject, 60).interrupts);
}

void test_gain() {
    CHECK_EQ(volume_scale_q8(0, false), 0);
    CHECK_EQ(volume_scale_q8(100, false), 256);
    // Muted is silent whatever the volume says - and mute must win, or the
    // switch is a lie.
    CHECK_EQ(volume_scale_q8(100, true), 0);
    CHECK_EQ(volume_scale_q8(-5, false), 0);
    CHECK_EQ(volume_scale_q8(1000, false), 256);
    // Perceptual, not linear: halfway along the slider is a quarter of full
    // power, so the top half of the travel still does something.
    CHECK_EQ(volume_scale_q8(50, false), 64);
    CHECK(volume_scale_q8(70, false) < 256);
    // Monotonic, so the slider never goes backwards.
    for (int v = 1; v <= 100; ++v) {
        CHECK(volume_scale_q8(v, false) >= volume_scale_q8(v - 1, false));
    }
}

void test_quiet_hours() {
    // Disabled by default [Q8].
    CHECK(!in_quiet_hours(0, 0, 0));
    CHECK(!in_quiet_hours(720, 0, 0));

    // A plain daytime window.
    CHECK(in_quiet_hours(10 * 60, 9 * 60, 17 * 60));
    CHECK(!in_quiet_hours(8 * 60, 9 * 60, 17 * 60));
    CHECK(!in_quiet_hours(17 * 60, 9 * 60, 17 * 60));   // end is exclusive
    CHECK(in_quiet_hours(9 * 60, 9 * 60, 17 * 60));     // start is inclusive

    // THE case: 22:00 to 07:00, which wraps midnight. A single comparison
    // gets this exactly backwards and would silence the display all day.
    const int s = 22 * 60, e = 7 * 60;
    CHECK(in_quiet_hours(23 * 60, s, e));
    CHECK(in_quiet_hours(0, s, e));
    CHECK(in_quiet_hours(3 * 60, s, e));
    CHECK(in_quiet_hours(22 * 60, s, e));
    CHECK(!in_quiet_hours(7 * 60, s, e));
    CHECK(!in_quiet_hours(12 * 60, s, e));
    CHECK(!in_quiet_hours(21 * 60 + 59, s, e));
}

}  // namespace

void run_tests() {
    test_a_good_file();
    test_chunks_are_walked();
    test_rejects_what_it_should();
    test_a_lying_header_is_clamped();
    test_cue_table();
    test_gain();
    test_quiet_hours();
}
