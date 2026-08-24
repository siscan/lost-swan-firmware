// Soak mode (spec 15 phase 6): drive the columns through thousands of wraps and
// report what happened.
//
// The point is not that it moves - everything else already proves that - but
// that it runs for HOURS and remembers.  A resync every few hundred revolutions
// is invisible in a five-second bench test and obvious after a night; heap that
// falls by a few hundred bytes an hour is a leak you cannot see any other way;
// and hall_to_hall drifting away from 8242 is the drum telling you the geometry
// in spec 3 is wrong.
//
// It drives motion:: directly, one flip at a time, closed loop - which is the
// path that has the edge verification in it.  It is deliberately NOT a mode:
// modes render time, and time is not a stress test.
#pragma once

#include <cstdint>

#include "motion/motion_types.h"

namespace swan {
namespace motion {

struct SoakColumn {
    uint32_t wraps = 0;         // completed revolutions of the ring
    uint32_t flips = 0;
    uint32_t resync_minor = 0;  // deltas since the soak started, not since boot
    uint32_t resync_major = 0;
    uint32_t faults = 0;
    int32_t h2h_min = 0;        // measured hall-to-hall extremes over the run
    int32_t h2h_max = 0;
    int32_t err_abs_max = 0;    // worst |last_hall_err| seen
};

struct SoakReport {
    bool running = false;
    uint32_t target_wraps = 0;
    uint32_t elapsed_s = 0;
    int32_t flaps_s = 0;
    SoakColumn col[N_COLUMNS];
    // Heap is sampled, not just read at the end: the shape over time is the
    // signal, and "it ended where it started" hides a sawtooth.
    uint32_t heap_start = 0;
    uint32_t heap_now = 0;
    uint32_t heap_min = 0;
    uint32_t samples = 0;
    // Set when the run stopped by itself, so `soak status` after a night says
    // whether it finished or something ended it.
    const char* stopped_because = "";
};

// `wraps` of 0 means "until stopped".  Returns false if a soak is already
// running, or if no column is in a state that can be driven.
bool soak_start(uint32_t wraps, int32_t flaps_s);
void soak_stop(const char* why);
bool soak_running();
SoakReport soak_report();

}  // namespace motion
}  // namespace swan
