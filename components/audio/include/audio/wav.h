// WAV parsing and the cue table (spec 9).  Pure - host-tested, no IDF.
//
// The player streams from LittleFS, so the assets are REPLACEABLE without a
// reflash, exactly as ring.json is: upload a WAV, it is validated into a
// staging path and renamed over the old one only once it parses.  Nico's real
// Swan recordings will land that way; the placeholders that ship are
// synthesized so the whole pipeline can be exercised before they exist.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace swan {
namespace audio {

// Spec 9: PCM WAV, 16-bit mono, 22050 Hz (16000 acceptable).
struct WavInfo {
    bool ok = false;
    uint32_t sample_rate = 0;
    uint16_t channels = 0;
    uint16_t bits = 0;
    uint32_t data_offset = 0;   // where the samples start
    uint32_t data_bytes = 0;
    const char* err = nullptr;
};

// Parse just the header.  `len` may be less than the whole file - the parser
// needs the chunk table only, and the player streams the rest.
WavInfo wav_parse(const uint8_t* data, std::size_t len);

// Cues (spec 9).  The countdown fires the first three; the UI ones are
// optional and silently absent if nobody supplied a file.
enum class CueId : uint8_t {
    Warn4Min = 0,
    Warn1Min,
    SystemFailure,
    UiExecute,
    UiReject,
    Count,
};
inline constexpr std::size_t CUE_COUNT = static_cast<std::size_t>(CueId::Count);

const char* cue_id_name(CueId c);
bool cue_id_from_name(std::string_view s, CueId& out);

// Where a cue's file lives.  One directory, one name per cue, so an upload
// knows exactly what it is replacing and a missing file is a missing cue
// rather than a mystery.
std::string cue_path(CueId c);

// Per-cue behaviour (spec 9): whether it loops, and for how long at most.
struct CuePolicy {
    bool loop = false;
    int max_loop_s = 0;
    bool interrupts = true;   // a new cue preempts the current one
};
CuePolicy cue_policy(CueId c, int failure_loop_s);

// Software gain, 0-100 to a Q8 multiplier (spec 9: no hardware volume).
// Perceptual rather than linear: a linear 50 sounds like most of full volume,
// which makes the slider useless over half its travel.
int16_t volume_scale_q8(int volume, bool muted);

// Quiet hours [Q8, default off].  Inclusive start, exclusive end, and it
// WRAPS: 22:00-07:00 is the case that matters and the one a naive comparison
// gets backwards.
bool in_quiet_hours(int minute_of_day, int start_min, int end_min);

}  // namespace audio
}  // namespace swan
