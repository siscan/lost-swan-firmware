// A modelled drum, cheap enough to run inside the 50 kHz step ISR.
//
// The host suite's SimDrum (test/host/sim_axis.h) computes hall_at() from
// scratch with 64-bit divisions.  That is fine on a desktop and impossible
// here for two separate reasons:
//
//   1. Cost.  Five columns x 50 kHz x several __udivdi3 calls is roughly a
//      third of the CPU, inside an ISR.
//   2. Placement.  __udivdi3 lives in FLASH.  Calling it from an IRAM ISR is
//      precisely what spec 5.2 forbids - an NVS or OTA write disables the
//      cache and the ISR stalls, dropping steps.
//
// So the edge position advances INCREMENTALLY, with the same integer DDA the
// step generator uses: one revolution is 272000/33 = 8242 usteps plus 14/33.
// The hot path is a bool test and two comparisons; the once-per-revolution
// advance is integer adds.  No division anywhere the ISR can reach.
//
// Pure - no IDF - so the host suite and the firmware model the same drum.
#pragma once

#include <cstdint>

#include "motion/motion_math.h"
#include "ring/geometry.h"

namespace swan {

struct SimDrum {
    // --- geometry, written by the control task ---
    int32_t rev_int = static_cast<int32_t>(USTEPS_PER_SPOOL_REV_NUM / USTEPS_PER_SPOOL_REV_DEN);
    int32_t rev_frac = static_cast<int32_t>(USTEPS_PER_SPOOL_REV_NUM % USTEPS_PER_SPOOL_REV_DEN);
    int32_t rev_den = static_cast<int32_t>(USTEPS_PER_SPOOL_REV_DEN);
    int32_t window = 60;   // operate window width in usteps
    int32_t jitter = 0;    // max +- per-edge jitter, usteps; does NOT accumulate

    // --- state, advanced by the ISR ---
    int64_t edge_pos = 0;  // pos_abs at which the current operate window opens
    int32_t edge_jit = 0;  // this edge's jitter offset
    int32_t frac = 0;      // fractional carry, in 1/rev_den usteps
    uint32_t edge_k = 0;   // edge counter, drives the deterministic jitter

    // --- injected faults (spec 5.9): set from the console, cleared the same way ---
    uint32_t suppress = 0;  // swallow this many upcoming edges; a big number is
                            // a dead sensor, and WHEN you inject it decides the
                            // signature - before a home it reads as NoHallEver,
                            // after one as a Jam
};

// Deterministic, no rand(): a simulation you cannot replay is not much use.
// The modulo is 32-bit, which RV32IMC does in one instruction - unlike the
// 64-bit divisions this class exists to avoid.
inline int32_t sim_drum_jitter_for(const SimDrum& d) {
    if (d.jitter <= 0) return 0;
    uint32_t h = d.edge_k * 2654435761u;
    h ^= h >> 15;
    const uint32_t span = static_cast<uint32_t>(2 * d.jitter + 1);
    return static_cast<int32_t>(h % span) - d.jitter;
}

// Place the first edge somewhere ahead of `pos`.  `start_angle` is how far
// past the previous edge the drum is sitting at power-up - a real assembly has
// no reason to start on one.
inline void sim_drum_reset(SimDrum& d, int64_t pos, int32_t start_angle) {
    const int32_t rev = d.rev_int;
    int32_t past = start_angle % (rev > 0 ? rev : 1);
    if (past < 0) past += rev;
    d.edge_pos = pos + (rev - past);
    d.frac = 0;
    d.edge_k = 0;
    d.edge_jit = sim_drum_jitter_for(d);
}

// Mechanical slip: the drum falls behind the motor, so every future edge
// arrives LATER in motor terms.  Exactly what a slipping pinion looks like to
// the edge-verification logic.
inline void sim_drum_slip(SimDrum& d, int32_t usteps) { d.edge_pos += usteps; }

// The hot path.  Called once per column per 20 us tick from an IRAM ISR:
// no division, no calls, and the while loop runs at most once per revolution
// because velocity is clamped to one ustep per tick.
SWAN_ALWAYS_INLINE bool sim_drum_hall(SimDrum& d, int64_t pos) {
    while (pos >= d.edge_pos + d.window) {
        d.edge_pos += d.rev_int;
        d.frac += d.rev_frac;
        if (d.frac >= d.rev_den) {
            d.frac -= d.rev_den;
            ++d.edge_pos;
        }
        ++d.edge_k;
        d.edge_jit = sim_drum_jitter_for(d);
        if (d.suppress > 0) --d.suppress;
    }
    if (d.suppress > 0) return false;
    const int64_t open = d.edge_pos + d.edge_jit;
    return pos >= open && pos < open + d.window;
}

}  // namespace swan
