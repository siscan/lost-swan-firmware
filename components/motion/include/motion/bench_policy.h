// The stand-in bench build's speed cap (spec 15 phase 8, BRINGUP 28b gate 3).
//
// THIS IS A SAFETY CONTRACT, NOT A CONFIG DEFAULT.  The §28b stand-in runs a
// real NEMA 17 inside a drum on a PRINTED PLA axle, before any purchase, to
// answer one question: does the motor cook itself in a sealed drum.  A PLA
// axle is fine at clock cadence and fine at a slow inspection spin.  It is not
// something to put the 400 flaps/s show spin through, and "we set the config
// low" is not a guarantee - a config value can be raised from the Settings
// page, from MQTT, or by an NVS record that survived from another build.
//
// So the cap is compiled in, it clamps rather than trusts, and there is no key
// that lifts it.  A build that can exceed it is a different build.
//
// Pure: no IDF, host-tested (test/host/test_bench_policy.cpp).
#pragma once

#include <cstdint>

#include "ring/geometry.h"

namespace swan {
namespace motion {

// One drum revolution per second.  The ring is 50 flaps, so 1 rev/s IS
// 50 flaps/s - which reads fast for flaps and is slow for a drum, and that is
// the point: it is a runout and wire-routing speed, not a show speed.
inline constexpr int32_t BENCH_MAX_FLAPS_S = static_cast<int32_t>(N_RING);

// What the show spin would be, for the comparison the refusal message makes.
inline constexpr int32_t SHOW_SPIN_FLAPS_S = 400;

static_assert(BENCH_MAX_FLAPS_S * 8 == SHOW_SPIN_FLAPS_S,
              "the cap is 1 drum rev/s and the show spin is 8 - if either "
              "changes, the refusal message below is lying about the ratio");

// True when this image is the stand-in bench build.  Compiled in, so nothing
// at runtime can make a normal image look like a bench one or the reverse.
#if defined(SWAN_BENCH) && SWAN_BENCH
inline constexpr bool BENCH_BUILD = true;
#else
inline constexpr bool BENCH_BUILD = false;
#endif

// The cap that applies to this image.  A normal build is limited only by the
// step ISR; the bench build is limited by the PLA.
constexpr int32_t bench_speed_cap() {
    return BENCH_BUILD ? BENCH_MAX_FLAPS_S : 0;  // 0 = no cap
}

// Clamp a commanded rate.  Used on every path that sets a speed, so a value
// that arrived from NVS, from a slider or from a peer cannot exceed the cap
// even if it was stored before this image was flashed.
constexpr int32_t bench_clamp_flaps_s(int32_t flaps_s) {
    const int32_t cap = bench_speed_cap();
    return (cap > 0 && flaps_s > cap) ? cap : flaps_s;
}

// Whether a rate is outright refused rather than quietly clamped.  A `spin`
// asks for a specific speed for a specific reason, so silently running it
// eight times slower than asked would be its own kind of lie - it is refused,
// with the number, and the operator decides.
constexpr bool bench_speed_refused(int32_t flaps_s) {
    const int32_t cap = bench_speed_cap();
    return cap > 0 && flaps_s > cap;
}

}  // namespace motion
}  // namespace swan
