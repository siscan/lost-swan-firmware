// Position model, ramp, DDA and edge verification.  Spec 3, 5.2-5.4.
#include <cstring>
#include <initializer_list>

#include "check.h"
#include "motion/motion_math.h"

using namespace swan;

namespace {

// --------------------------------------------------------------------------
// T(i) and the non-integer ratio (spec 3)
// --------------------------------------------------------------------------
void test_geometry_constants() {
    // DIRECT DRIVE, 1:1 (docs/ref/DRIVE_CHANGE.md).  Every one of these is now
    // an exact integer; under the old 85T/33T rim gear none of them were.
    CHECK_EQ(USTEPS_PER_MOTOR_REV, 3200);
    CHECK_EQ(USTEPS_PER_FLAP_NUM, 64);
    CHECK_EQ(USTEPS_PER_FLAP_DEN, 1);
    CHECK_EQ(USTEPS_PER_SPOOL_REV_NUM, 3200);
    CHECK_EQ(USTEPS_PER_SPOOL_REV_DEN, 1);
    CHECK_EQ(USTEPS_PER_SPOOL_REV_NOMINAL, 3200);

    // The speed table in spec 3, recomputed rather than copied.
    CHECK_EQ(flaps_s_to_usteps_s(8), 512);
    CHECK_EQ(flaps_s_to_usteps_s(15), 960);
    CHECK_EQ(flaps_s_to_usteps_s(20), 1280);
    CHECK_EQ(flaps_s_to_usteps_s(25), 1600);

    // The show spin, which is the number that decided the step-gen
    // architecture (spec 5.2).  At the old ratio this was ~66k usteps/s per
    // column against a hard TICK_HZ ceiling of 50k - not reachable from a
    // timer ISR, and not reachable with peripherals either, because the C5 has
    // two RMT TX channels for five axes.  At 1:1 it fits with room to spare.
    CHECK_EQ(flaps_s_to_usteps_s(400), 25600);
    // The ceiling that matters is PER AXIS: the DDA emits at most one step per
    // ISR tick, so no axis can exceed TICK_HZ however many axes there are.
    // 25,600 of a 50,000 ceiling: 51.2% duty.  Comfortable, but NOT the 2x
    // margin it would be tempting to claim - the test says so rather than the
    // comment, because the honest number is what the bench has to live with.
    CHECK(flaps_s_to_usteps_s(400) <= TICK_HZ);
    CHECK(flaps_s_to_usteps_s(400) * 100 / TICK_HZ == 51);
}

void test_ring_target() {
    CHECK_EQ(ring_target_usteps(0), 0);
    CHECK_EQ(ring_target_usteps(1), 64);     // exact at 1:1
    CHECK_EQ(ring_target_usteps(50), 3200);  // exact at 1:1

    // Strictly increasing, and never more than half a ustep from the true
    // value.  Written against the CONSTANTS rather than against 5440/33, so it
    // keeps its meaning through a geometry change:
    //   |T(i) - i*NUM/DEN| <= 1/2   <=>   |2*DEN*T(i) - 2*i*NUM| <= DEN.
    for (int64_t i = 1; i <= 200; ++i) {
        CHECK(ring_target_usteps(i) > ring_target_usteps(i - 1));
        const int64_t resid = 2 * USTEPS_PER_FLAP_DEN * ring_target_usteps(i) -
                              2 * i * USTEPS_PER_FLAP_NUM;
        CHECK(resid <= USTEPS_PER_FLAP_DEN && resid >= -USTEPS_PER_FLAP_DEN);
    }

    // At 1:1 every revolution is EXACTLY 3200 usteps and the residue that the
    // rim gear carried is identically zero.
    for (int64_t i = 0; i < 50; ++i) {
        CHECK_EQ(ring_target_usteps(i + 50) - ring_target_usteps(i), 3200);
    }
}

// The rounding machinery is kept although 1:1 makes it exact (spec 3: exactness
// is a bonus, not a licence to delete it).  Kept code that nothing exercises is
// how a future geometry change lands on an untested path - so it is exercised
// here with the ratio it actually used to have, 85T/33T, where the answers are
// known and were checked against the bench plan.
void test_fractional_machinery_still_works() {
    // The same expression as ring_target_usteps, with the ratio as parameters.
    const auto target_with = [](int64_t i, int64_t num, int64_t den) {
        return (2 * i * num + den) / (2 * den);
    };
    constexpr int64_t NUM = 5440, DEN = 33;   // the dead 85T/33T rim gear

    CHECK_EQ(target_with(0, NUM, DEN), 0);
    CHECK_EQ(target_with(1, NUM, DEN), 165);    // round(164.8485)
    CHECK_EQ(target_with(50, NUM, DEN), 8242);  // round(8242.4242)

    // Half-ustep bound holds under a genuinely fractional ratio.
    for (int64_t i = 1; i <= 200; ++i) {
        CHECK(target_with(i, NUM, DEN) > target_with(i - 1, NUM, DEN));
        const int64_t resid = 2 * DEN * target_with(i, NUM, DEN) - 2 * i * NUM;
        CHECK(resid <= DEN && resid >= -DEN);
    }

    // And the 0.42-ustep-per-rev residue alternates 8242/8243 exactly as it did
    // - the behaviour spec 5.3's edge re-basing exists to absorb.
    bool saw_8242 = false, saw_8243 = false;
    for (int64_t i = 0; i < 50; ++i) {
        const int64_t rev = target_with(i + 50, NUM, DEN) - target_with(i, NUM, DEN);
        CHECK(rev == 8242 || rev == 8243);
        if (rev == 8242) saw_8242 = true;
        if (rev == 8243) saw_8243 = true;
    }
    CHECK(saw_8242);
    CHECK(saw_8243);
}

// The reason T(i) is computed from i and never accumulated (handoff 1: naive
// per-flip rounding drifts several flaps an hour).  This is the regression
// guard on that whole decision.
void test_no_accumulation() {
    constexpr int64_t FLAPS = 2500;  // 50 revolutions

    // At 1:1 accumulation and computation agree exactly, because a flap is a
    // whole number of usteps.  Asserted so the equality is a stated property
    // rather than a coincidence nobody checked.
    const int64_t exact = ring_target_usteps(FLAPS);
    CHECK_EQ(exact, FLAPS * 64);
    int64_t naive = 0;
    for (int64_t i = 0; i < FLAPS; ++i) naive += ring_target_usteps(1);
    CHECK_EQ(naive, exact);

    // AND THE RULE STILL HAS TEETH.  The direct drive made this particular
    // failure impossible, it did not make the rule wrong - T(i) is still
    // computed from i, and the moment a geometry is not integral the drift
    // comes back.  Demonstrated on the ratio the machine actually had:
    const auto target_with = [](int64_t i, int64_t num, int64_t den) {
        return (2 * i * num + den) / (2 * den);
    };
    constexpr int64_t NUM = 5440, DEN = 33;          // the dead 85T/33T rim gear
    const int64_t exact_frac = target_with(FLAPS, NUM, DEN);
    CHECK_EQ(exact_frac, 412121);
    int64_t naive_frac = 0;
    for (int64_t i = 0; i < FLAPS; ++i) naive_frac += target_with(1, NUM, DEN);
    CHECK_EQ(naive_frac, 412500);                    // 165 each, accumulated
    CHECK(naive_frac - exact_frac > 2 * target_with(1, NUM, DEN));
}

// --------------------------------------------------------------------------
// Position model (spec 5.3)
// --------------------------------------------------------------------------
void test_plan_target() {
    const int64_t hall = 100000;
    const int32_t cal = 37;

    // At rest on index 0, a move forward lands on that index in this revolution.
    const int64_t pos0 = index_position(hall, cal, 0);
    CHECK_EQ(pos0, hall + cal);
    CHECK_EQ(plan_target(hall, cal, pos0, 7), hall + cal + ring_target_usteps(7));

    // Asking for the index we already show is a zero-distance move, not a wrap.
    CHECK_EQ(plan_target(hall, cal, pos0, 0), pos0);

    // A move that goes "backwards" on the ring is a forward wrap: from index 40
    // to index 5 the target is index 5 of the next revolution (to within the
    // 1-ustep rounding of the nominal-revolution wrap).
    const int64_t pos40 = index_position(hall, cal, 40);
    const int64_t t = plan_target(hall, cal, pos40, 5);
    const int64_t wrap_err = t - (hall + cal + ring_target_usteps(55));
    CHECK(wrap_err >= -1 && wrap_err <= 1);
    CHECK(t > pos40);
    // ...and it costs exactly (5 - 40) mod 50 = 15 flips of travel.
    CHECK_EQ(ring_forward_distance(40, 5), 15);

    // Every from/to pair must produce a forward-only target of the right length.
    for (int from = 0; from < RING_SLOT_COUNT; ++from) {
        const int64_t pos = index_position(hall, cal, from);
        for (int to = 0; to < RING_SLOT_COUNT; ++to) {
            const int64_t tgt = plan_target(hall, cal, pos, to);
            CHECK(tgt >= pos);
            const int flips = ring_forward_distance(from, to);
            // Distance matches the flip count to within endpoint rounding plus
            // the nominal-revolution wrap (each contributes at most 1 ustep;
            // any residue is absorbed at the next Hall edge).
            const int64_t nominal = ring_target_usteps(from + flips) - ring_target_usteps(from);
            const int64_t err = (tgt - pos) - nominal;
            CHECK(err <= 2 && err >= -2);
        }
    }
}

// The anchor offset cal + T(to) reduced into [0, one revolution).  Without
// this reduction, a mid-move edge rebase with cal + T(to) > rev pushed the
// target past the NEXT edge and go() never terminated (see test_axis_sim).
void test_edge_anchor_offset() {
    CHECK_EQ(edge_anchor_offset(0, 0), 0);
    CHECK_EQ(edge_anchor_offset(40, 5), 40 + ring_target_usteps(5));
    // Cases where cal + T(to) exceeds a revolution and must reduce.  Written
    // out arithmetically so a geometry change makes them fail loudly rather
    // than silently testing nothing.
    CHECK_EQ(edge_anchor_offset(3100, 49), 3100 + 3136 - 3200);  // 3036
    CHECK_EQ(edge_anchor_offset(200, 49), 200 + 3136 - 3200);    // 136
    CHECK_EQ(edge_anchor_offset(3199, 49), 3199 + 3136 - 3200);  // 3135, once not twice

    // The invariant the termination proof rests on: for EVERY legal cal and
    // dest, the anchor offset is sub-revolution, so a rebased target can never
    // recede across successive edges.
    for (int32_t cal = 0; cal < USTEPS_PER_SPOOL_REV_NOMINAL; cal += 97) {
        for (int to = 0; to < RING_SLOT_COUNT; ++to) {
            const int64_t e = edge_anchor_offset(cal, to);
            CHECK(e >= 0 && e < USTEPS_PER_SPOOL_REV_NOMINAL);
            // And retarget_on_edge built on it stays within one revolution of
            // the edge (before the forward clamp).
            const int64_t t = retarget_on_edge(1000000, cal, 1000000, to);
            CHECK(t - 1000000 < USTEPS_PER_SPOOL_REV_NOMINAL);
        }
    }
}

void test_retarget_on_edge() {
    const int32_t cal = 37;

    // Mid-move across home: the provisional target was based on the old edge.
    const int64_t hall_old = 0;
    const int64_t pos = index_position(hall_old, cal, 40);
    const int64_t provisional = plan_target(hall_old, cal, pos, 5);

    // The real edge lands 3 usteps late.  The target rebases onto it.
    const int64_t hall_new = USTEPS_PER_SPOOL_REV_NOMINAL + 3;
    const int64_t corrected = retarget_on_edge(hall_new, cal, hall_new, 5);
    CHECK_EQ(corrected, hall_new + cal + ring_target_usteps(5));
    CHECK(corrected != provisional);  // the correction actually did something

    // Rounding is re-zeroed every revolution, so the correction stays tiny.
    const int64_t drift = corrected - provisional;
    CHECK(drift <= 4 && drift >= -4);

    // Forward-only: if we have already overshot, the target clamps to here
    // rather than commanding a reverse move.
    const int64_t past = hall_new + cal + ring_target_usteps(5) + 500;
    CHECK_EQ(retarget_on_edge(hall_new, cal, past, 5), past);
}

// --------------------------------------------------------------------------
// Edge verification (spec 5.4)
// --------------------------------------------------------------------------
void test_edge_classification() {
    const EdgeTolerances tol = DEFAULT_EDGE_TOLERANCES;
    // Both derive from the flap, so they followed the drive change: a quarter
    // flap is 16 usteps now, and a flap is 64.
    CHECK_EQ(tol.silent, 16);
    CHECK_EQ(tol.major, 64);

    CHECK(classify_edge_error(0, tol) == EdgeVerdict::Minor);
    CHECK(classify_edge_error(16, tol) == EdgeVerdict::Minor);
    CHECK(classify_edge_error(-16, tol) == EdgeVerdict::Minor);
    CHECK(classify_edge_error(17, tol) == EdgeVerdict::Major);
    CHECK(classify_edge_error(64, tol) == EdgeVerdict::Major);
    CHECK(classify_edge_error(-64, tol) == EdgeVerdict::Major);
    CHECK(classify_edge_error(65, tol) == EdgeVerdict::Fault);
    CHECK(classify_edge_error(-65, tol) == EdgeVerdict::Fault);

    // At 1:1 a clean revolution measures EXACTLY 3200 - there is no residue to
    // alternate any more, which is itself the strongest single check that the
    // drum on the bench is the direct-drive one.
    CHECK_EQ(edge_error(1000, 1000 + 3200), 0);
    CHECK_EQ(edge_error(1000, 1000 + 3201), 1);
    CHECK(classify_edge_error(edge_error(1000, 1000 + 3201), tol) == EdgeVerdict::Minor);

    // WRONG-DRUM SIGNATURES (geometry.h has the pedigree).  A drum still on the
    // 85T/33T rim gear, read by direct-drive firmware, is out by 5042 usteps a
    // revolution - 78 flaps.  That is not a resync, it is an immediate Fault,
    // and it is unmistakable, which is the point: the two machines can no
    // longer be confused for one another by a marginal number.
    CHECK_EQ(edge_error(0, 8242), 5042);
    CHECK(classify_edge_error(edge_error(0, 8242), tol) == EdgeVerdict::Fault);
    // The never-built 36T revision would have read 7555 - equally unmistakable.
    CHECK(classify_edge_error(edge_error(0, 7555), tol) == EdgeVerdict::Fault);

    // A genuine Major is a much smaller number now, because a flap is 64
    // usteps rather than 165: past the silent tolerance but inside one flap.
    CHECK(classify_edge_error(tol.silent + 1, tol) == EdgeVerdict::Major);

    // The missed-edge window is a revolution and a HALF, not a revolution and
    // a flap.  At the old width a slip of just over one flap and a drum that
    // had stopped dead were the same observation - "the edge is a little
    // overdue" - and they need opposite responses.  Wide enough that a slip
    // arrives as a late edge (Slip, retried) and only a real absence trips it
    // (Jam, never retried).
    CHECK(!edge_overdue(0, 0));
    CHECK(!edge_overdue(3200, 0));
    CHECK(!edge_overdue(3200 + 64, 0));     // a one-flap slip is still just late
    CHECK(!edge_overdue(3200 + 1500, 0));   // so is a big one
    CHECK(edge_overdue(3200 + 1700, 0));    // past 1.5 revolutions: it stopped
}

// --------------------------------------------------------------------------
// Ramp + DDA (spec 5.2)
// --------------------------------------------------------------------------
struct SimResult {
    int64_t steps;
    int32_t peak_v;
    int64_t isr_ticks;
    bool finished;
};

// Runs the real control tick and the real ISR DDA against each other.
SimResult simulate_move(int64_t distance, int32_t v_max, int32_t accel) {
    const RampParams rp{v_max, accel, CONTROL_HZ};
    const int ratio = TICK_HZ / CONTROL_HZ;  // control ticks are every 50th ISR tick

    int64_t pos = 0;
    int32_t v = 0;
    uint32_t accum = 0;
    int32_t peak = 0;
    int64_t ticks = 0;
    const int64_t limit = 200LL * TICK_HZ;  // 200 s of simulated time

    while (pos < distance && ticks < limit) {
        if (ticks % ratio == 0) {
            v = ramp_next_velocity(distance - pos, v, rp);
            if (v > peak) peak = v;
        }
        if (v > 0 && pos < distance) {
            if (dda_tick(accum, v)) ++pos;
        }
        ++ticks;
    }
    return {pos, peak, ticks, pos == distance};
}

void test_dda_rate() {
    // At 4121 usteps/s the DDA must emit exactly 4121 steps in one second.
    uint32_t accum = 0;
    int64_t steps = 0;
    for (int32_t t = 0; t < TICK_HZ; ++t) {
        if (dda_tick(accum, 4121)) ++steps;
    }
    CHECK_EQ(steps, 4121);

    // And never more than one step per tick, which is what lets all five axes
    // share a single bank write.
    accum = 0;
    for (int32_t t = 0; t < 1000; ++t) {
        const bool a = dda_tick(accum, TICK_HZ - 1);
        (void)a;  // at most one step - guaranteed by the single subtract
    }
    CHECK(accum < static_cast<uint32_t>(TICK_HZ));
}

void test_ramp_arrives_exactly() {
    const int32_t v_alarm = flaps_s_to_usteps_s(25);
    const int32_t accel = 82000;

    // A full 49-flip wrap: the worst case in the spec 3 table.
    const int64_t wrap = ring_target_usteps(49);
    SimResult r = simulate_move(wrap, flaps_s_to_usteps_s(20), accel);
    CHECK(r.finished);
    CHECK_EQ(r.steps, wrap);
    CHECK(r.peak_v <= flaps_s_to_usteps_s(20));
    // Spec 3 says ~2.45 s at 20 flaps/s; allow for the ramp at each end.
    const double seconds = static_cast<double>(r.isr_ticks) / TICK_HZ;
    CHECK(seconds > 2.3 && seconds < 2.8);

    // Single flip, and a single ustep: short moves must still land exactly.
    for (int64_t d : {int64_t{1}, int64_t{2}, ring_target_usteps(1), wrap,
                      USTEPS_PER_SPOOL_REV_NOMINAL}) {
        SimResult s = simulate_move(d, v_alarm, accel);
        CHECK(s.finished);
        CHECK_EQ(s.steps, d);
        CHECK(s.peak_v <= v_alarm);
    }

    // A short move is triangular: it never reaches cruise speed.  The distance
    // needed to reach cruise fell with the speed - v^2/2a is ~16 usteps at 1:1
    // against ~104 under the rim gear - so this has to be a shorter move than
    // it used to be to still be testing what it says.
    SimResult tri = simulate_move(20, v_alarm, accel);
    CHECK(tri.finished);
    CHECK(tri.peak_v < v_alarm);

    // 0 -> alarm speed in about 20 ms.
    //
    // IT USED TO BE ~50 ms AT THE SAME `accel`, and that difference is a real
    // mechanical change rather than a test detail: alarm speed is now 1600
    // usteps/s instead of 4121, so the same 82000 usteps/s^2 reaches it 2.6x
    // sooner - which at 1:1 is 2.6x the DRUM angular acceleration, against a
    // reflected torque demand that also rose (no gear reduction).  The bench
    // re-derives `motion.accel`; spec 5.2 records that it is now open.
    int32_t v = 0;
    int ticks_to_full = 0;
    while (v < v_alarm && ticks_to_full < 10000) {
        v = ramp_next_velocity(1000000, v, RampParams{v_alarm, accel, CONTROL_HZ});
        ++ticks_to_full;
    }
    CHECK(ticks_to_full >= 18 && ticks_to_full <= 22);  // control ticks == ms
}

void test_ramp_edges() {
    const RampParams rp{1600, 82000, CONTROL_HZ};
    CHECK_EQ(ramp_next_velocity(0, 1600, rp), 0);   // arrived
    CHECK_EQ(ramp_next_velocity(-5, 100, rp), 0);   // overshot
    CHECK(ramp_next_velocity(100000, 0, rp) > 0);   // starts from rest
    // Velocity is clamped so the DDA can never be asked for >1 step per tick.
    CHECK(ramp_next_velocity(1000000, TICK_HZ, RampParams{TICK_HZ * 4, 82000, CONTROL_HZ}) <=
          TICK_HZ);
}

}  // namespace

void run_tests() {
    test_geometry_constants();
    test_ring_target();
    test_fractional_machinery_still_works();
    test_no_accumulation();
    test_plan_target();
    test_edge_anchor_offset();
    test_retarget_on_edge();
    test_edge_classification();
    test_dda_rate();
    test_ramp_arrives_exactly();
    test_ramp_edges();
}
