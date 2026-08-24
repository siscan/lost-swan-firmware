#include "audio/wav.h"

#include <cstring>

namespace swan {
namespace audio {
namespace {

uint16_t rd16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
bool tag(const uint8_t* p, const char* t) { return std::memcmp(p, t, 4) == 0; }

}  // namespace

WavInfo wav_parse(const uint8_t* data, std::size_t len) {
    WavInfo w;
    if (data == nullptr || len < 44) {
        w.err = "too short to be a WAV";
        return w;
    }
    if (!tag(data, "RIFF") || !tag(data + 8, "WAVE")) {
        w.err = "not a RIFF/WAVE file";
        return w;
    }

    // Walk the chunk table rather than assuming the canonical 44-byte layout:
    // plenty of encoders put LIST/INFO or a fact chunk between fmt and data,
    // and a fixed offset would read metadata as audio.
    std::size_t i = 12;
    bool have_fmt = false;
    while (i + 8 <= len) {
        const uint32_t size = rd32(data + i + 4);
        const std::size_t body = i + 8;
        if (tag(data + i, "fmt ")) {
            if (size < 16 || body + 16 > len) {
                w.err = "truncated fmt chunk";
                return w;
            }
            const uint16_t format = rd16(data + body);
            w.channels = rd16(data + body + 2);
            w.sample_rate = rd32(data + body + 4);
            w.bits = rd16(data + body + 14);
            // 1 = PCM. 0xFFFE is WAVE_FORMAT_EXTENSIBLE, which may still be
            // PCM, but the subformat GUID would have to be checked and nothing
            // this device plays needs it.
            if (format != 1) {
                w.err = "not uncompressed PCM";
                return w;
            }
            have_fmt = true;
        } else if (tag(data + i, "data")) {
            if (!have_fmt) {
                w.err = "data chunk before fmt";
                return w;
            }
            w.data_offset = static_cast<uint32_t>(body);
            // Trust the file's length over the header's: a truncated upload
            // announces the full size, and streaming past the end would play
            // whatever follows in the filesystem.
            const std::size_t avail = len > body ? len - body : 0;
            w.data_bytes = static_cast<uint32_t>(size < avail ? size : avail);
            break;
        }
        // Chunks are word-aligned; an odd size is followed by a pad byte.
        i = body + size + (size & 1u);
    }

    if (!have_fmt) {
        w.err = "no fmt chunk";
        return w;
    }
    if (w.data_offset == 0) {
        w.err = "no data chunk";
        return w;
    }
    if (w.channels != 1) {
        w.err = "not mono (spec 9)";
        return w;
    }
    if (w.bits != 16) {
        w.err = "not 16-bit (spec 9)";
        return w;
    }
    // The I2S clock is reconfigured per file, so any sane rate would play -
    // but the amp is a 40 mm speaker in a wooden box and the LittleFS budget
    // is shared with the web UI, so the spec's range is enforced rather than
    // letting a 48 kHz stereo master quietly fill the partition.
    if (w.sample_rate < 8000 || w.sample_rate > 32000) {
        w.err = "sample rate outside 8000-32000 (spec 9: 22050, or 16000)";
        return w;
    }
    if (w.data_bytes == 0) {
        w.err = "no samples";
        return w;
    }
    w.ok = true;
    return w;
}

const char* cue_id_name(CueId c) {
    switch (c) {
        case CueId::Warn4Min:      return "warn_4min";
        case CueId::Warn1Min:      return "warn_1min";
        case CueId::SystemFailure: return "system_failure";
        case CueId::UiExecute:     return "ui_execute";
        case CueId::UiReject:      return "ui_reject";
        case CueId::Count:         break;
    }
    return "?";
}

bool cue_id_from_name(std::string_view s, CueId& out) {
    for (std::size_t i = 0; i < CUE_COUNT; ++i) {
        const CueId c = static_cast<CueId>(i);
        if (s == cue_id_name(c)) {
            out = c;
            return true;
        }
    }
    return false;
}

std::string cue_path(CueId c) { return std::string("/fs/audio/") + cue_id_name(c) + ".wav"; }

CuePolicy cue_policy(CueId c, int failure_loop_s) {
    CuePolicy p;
    if (c == CueId::SystemFailure) {
        // The alarm loops, bounded by countdown.failure_loop_s (default 60).
        // Unbounded would mean a display in an empty house screaming until
        // somebody comes home.
        p.loop = true;
        p.max_loop_s = failure_loop_s;
    }
    if (c == CueId::UiExecute || c == CueId::UiReject) {
        // A key click must never cut off the four-minute warning.
        p.interrupts = false;
    }
    return p;
}

int16_t volume_scale_q8(int volume, bool muted) {
    if (muted || volume <= 0) return 0;
    if (volume > 100) volume = 100;
    // Roughly perceptual: gain = (v/100)^2.  A linear scale spends its top
    // half sounding identical, so the slider is useless above about 50.
    const int32_t g = (static_cast<int32_t>(volume) * volume * 256) / 10000;
    return static_cast<int16_t>(g > 256 ? 256 : g);
}

bool in_quiet_hours(int minute_of_day, int start_min, int end_min) {
    if (start_min == end_min) return false;   // disabled [Q8 default]
    if (start_min < end_min) return minute_of_day >= start_min && minute_of_day < end_min;
    // Wrapped across midnight - 22:00 to 07:00 - which is the case anyone
    // actually configures and the one a single comparison gets backwards.
    return minute_of_day >= start_min || minute_of_day < end_min;
}

}  // namespace audio
}  // namespace swan
