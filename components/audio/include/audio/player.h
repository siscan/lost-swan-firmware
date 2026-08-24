// The cue player (spec 9) - the IDF shell.  Policy is pure, in audio/wav.h.
//
// One file at a time, streamed from LittleFS to I2S via DMA.  Assets are
// REPLACEABLE without a reflash, like ring.json: upload a WAV, it is parsed and
// only then renamed over the old one.  The placeholders that ship are
// synthesized (tools/gen_audio.py) so the whole pipeline works before Nico's
// recordings exist.
#pragma once

#include <cstdint>
#include <string>

#include "audio/wav.h"
#include "esp_err.h"

namespace swan {
namespace audio {

struct AudioSettings {
    int volume = 70;        // [Q8 default]
    bool mute = false;
    int quiet_start_min = 0;   // both zero = quiet hours off [Q8 default]
    int quiet_end_min = 0;
    int failure_loop_s = 60;   // countdown.failure_loop_s
};

esp_err_t init(const AudioSettings& s);

// Play a cue.  Non-blocking: it posts to the player task, so a caller on the
// modes task (which is where cues fire) never waits on DMA or on LittleFS.
//
// `minute_of_day` is local time, for quiet hours; -1 means "unknown", which
// plays - a display whose clock has not synced should still be able to make a
// noise, and silence would be indistinguishable from a broken amp.
void play(CueId c, int minute_of_day);
void stop();

void set_settings(const AudioSettings& s);
AudioSettings settings();

struct Status {
    bool playing = false;
    std::string cue;
    bool have[CUE_COUNT] = {};   // which cue files are present and parse
    uint32_t underruns = 0;
};
Status status();

// Re-scan /fs/audio after an upload.
void rescan();

}  // namespace audio
}  // namespace swan
