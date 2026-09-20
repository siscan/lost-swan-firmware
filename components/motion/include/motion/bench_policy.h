// The stand-in bench build's speed cap (spec 15 phase 8, BRINGUP 28b/28c).
//
// THIS IS A SAFETY CONTRACT, NOT A CONFIG DEFAULT.  The §28b stand-in runs a
// real NEMA 17 inside a drum on a PRINTED PLA axle, before any purchase, to
// answer one question: does the motor cook itself in a sealed drum.  A PLA
// axle is fine at clock cadence and fine at a slow inspection spin.  It is not
// something to put the 400 flaps/s show spin through, and "we set the config
// low" is not a guarantee - a config value can be raised from the Settings
// page, from MQTT, or by an NVS record that survived from another build.
//
// So the cap is compiled in, it REFUSES rather than trusting, and there is no
// key that lifts it.  A build that can exceed it is a different build.
//
// Pure: no IDF, host-tested (test/host/test_bench_policy.cpp).
#pragma once

#include <cstdint>

#include "ring/geometry.h"

namespace swan {
namespace motion {

// THE CEILING, which no build may exceed: one drum revolution per second.  The
// ring is 50 flaps, so 1 rev/s IS 50 flaps/s - which reads fast for flaps and
// is slow for a drum, and that is the point: it is a runout and wire-routing
// speed, not a show speed.
inline constexpr int32_t BENCH_CAP_CEILING = static_cast<int32_t>(N_RING);

// What the show spin would be, for the comparison the refusal message makes.
inline constexpr int32_t SHOW_SPIN_FLAPS_S = 400;

static_assert(BENCH_CAP_CEILING * 8 == SHOW_SPIN_FLAPS_S,
              "the ceiling is 1 drum rev/s and the show spin is 8 - if either "
              "changes, the refusal message is lying about the ratio");

// THE CAP THIS IMAGE CARRIES.  `-DSWAN_BENCH_CAP=<flaps/s>` at configure time;
// the default is the ceiling.
//
// WHY THIS IS A BUILD PARAMETER AND NOT A CONSTANT (2026-09-20).  The cap is
// not one number, it is "the fastest this PARTICULAR mechanism may be driven",
// and the mechanism changes between sessions while the firmware does not.  The
// pitch-80 module arriving for §28c has NO SHROUD: the cards lift off the drum
// somewhere above ~100 flaps/s and there is nothing to stop one leaving.  50 is
// a perfectly good number for a bare stand-in drum and a bad one for a loaded
// unshrouded module, and the difference must not be a runtime setting somebody
// can drag - it is the same argument that made the cap compiled in at all.
//
// IT MAY ONLY GO DOWN.  The static_assert below is the whole of "do not add a
// way to lift the cap": a parameter that can only lower a compiled-in ceiling
// is not a way to lift it, and CMake refuses an out-of-range value at configure
// time so the failure is legible rather than a template error.
//
// And it is in the VERSION STRING (`0.4.0+devkitc1.bench20`), because two
// images with different caps are two different safety contracts and
// esp_app_desc_t has nowhere else to say so - the same reason the board map and
// the sim flavour are there.
#if defined(SWAN_BENCH_CAP)
inline constexpr int32_t BENCH_MAX_FLAPS_S = static_cast<int32_t>(SWAN_BENCH_CAP);
#else
inline constexpr int32_t BENCH_MAX_FLAPS_S = BENCH_CAP_CEILING;
#endif

static_assert(BENCH_MAX_FLAPS_S >= 1,
              "SWAN_BENCH_CAP below 1 flap/s is a build that cannot turn a drum");
static_assert(BENCH_MAX_FLAPS_S <= BENCH_CAP_CEILING,
              "SWAN_BENCH_CAP may only LOWER the cap. The ceiling is one drum "
              "revolution per second and it is a safety contract, not a default");

// True when this image is the stand-in bench build.  Compiled in, so nothing
// at runtime can make a normal image look like a bench one or the reverse.
#if defined(SWAN_BENCH) && SWAN_BENCH
inline constexpr bool BENCH_BUILD = true;
#else
inline constexpr bool BENCH_BUILD = false;
#endif

// The cap that applies to this image.  A normal build is limited only by the
// step ISR; the bench build is limited by whatever mechanism is on the vise.
constexpr int32_t bench_speed_cap() {
    return BENCH_BUILD ? BENCH_MAX_FLAPS_S : 0;  // 0 = no cap
}

// REFUSED, NEVER CLAMPED, ON EVERY PATH.  A speed is asked for by somebody for
// a reason - a ladder rung, a runout spin, an alarm setting - and running it
// two or eight times slower than asked without saying so is its own kind of
// lie.  It is also the lie that would make the §28c ladder useless: a rung
// silently executed at the cap would report edge errors for a speed nobody ran.
//
// This used to be paired with a `bench_clamp_flaps_s` that set_params called
// silently.  That function is GONE rather than merely unused: a clamp sitting
// in the header is a trap still loaded, and the whole point of the 2026-09-11
// entry is that the cap was applied in the place speeds are CONFIGURED and not
// in the place they are commanded.
constexpr bool bench_speed_refused(int32_t flaps_s) {
    const int32_t cap = bench_speed_cap();
    return cap > 0 && flaps_s > cap;
}

// THE ONE EXCEPTION, AND IT IS THE BOOT PATH.
//
// A refusal needs somebody to refuse to.  At boot there is nobody: the stored
// speeds arrive from NVS written by another build, or from the compiled
// defaults (which are the DISPLAY's - 25 flaps/s alarm - and belong to no
// particular bench), and an image that came up with none of its motion config
// applied would be a worse answer than one that says which value it could not
// honour.
//
// So the boot paths SUBSTITUTE, and `changed` is what makes it a substitution
// rather than a silent clamp: every caller logs the field, the value and the
// cap.  Nothing else in the firmware may call this - everywhere a person or a
// peer asks for a speed, bench_speed_refused is the rule.
constexpr int32_t bench_enforced(int32_t want, bool& changed) {
    if (!bench_speed_refused(want)) return want;
    changed = true;
    return BENCH_MAX_FLAPS_S;
}

}  // namespace motion
}  // namespace swan
