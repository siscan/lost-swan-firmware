// The physical button's gesture recogniser (spec §2 Q6).
//
// Every rule here exists because of the hardware: GPIO28 is the BOOT strapping
// pin, so a press held across a reset is a gesture the firmware must survive
// rather than obey.
#include <cstdio>
#include <initializer_list>

#include "button/gesture.h"
#include "check.h"

using namespace swan;
using namespace swan::button;

namespace {

// Drive the recogniser at a fixed cadence, the way the task does, and return
// the gestures it produced.
struct Runner {
    ButtonGesture g;
    int64_t now = 0;
    int step_ms = 10;
    int presses = 0;
    int holds = 0;

    explicit Runner(GestureConfig cfg = {}) : g(cfg) {}

    void run(bool pressed, int ms) {
        for (int t = 0; t < ms; t += step_ms) {
            const Gesture r = g.feed(pressed, now);
            if (r == Gesture::Press) ++presses;
            if (r == Gesture::Hold) ++holds;
            now += step_ms;
        }
    }
};

void test_a_short_press_is_one_execute() {
    Runner r;
    r.run(false, 200);          // idle
    r.run(true, 300);           // pressed, well past debounce, short of hold
    r.run(false, 200);          // released
    CHECK_EQ(r.presses, 1);
    CHECK_EQ(r.holds, 0);
}

void test_a_long_press_is_one_rehome_and_no_execute() {
    Runner r;
    r.run(false, 200);
    r.run(true, 2500);          // past the 2 s hold threshold
    r.run(false, 300);          // and released afterwards
    // The hold fires ONCE, while still held, and the release must NOT also
    // report a press - otherwise every re-home is followed by a countdown.
    CHECK_EQ(r.holds, 1);
    CHECK_EQ(r.presses, 0);
}

void test_a_very_long_press_still_fires_once() {
    Runner r;
    r.run(false, 100);
    r.run(true, 30000);         // leaning on it
    r.run(false, 100);
    CHECK_EQ(r.holds, 1);
    CHECK_EQ(r.presses, 0);
}

// THE SAFETY RULE.  GPIO28 is the BOOT strap: holding the button across a reset
// is exactly the gesture that can land the board in the serial bootloader, and
// on the boot that DOES come up the firmware must not then act on the hold it
// inherited.  Nothing is produced until the button has been released once.
void test_a_button_held_at_startup_is_ignored_until_released() {
    Runner r;
    CHECK(!r.g.armed());
    r.run(true, 10000);         // already down when we started, and stays down
    CHECK_EQ(r.presses, 0);
    CHECK_EQ(r.holds, 0);       // not even the hold
    CHECK(!r.g.armed());

    r.run(false, 200);          // let go
    CHECK(r.g.armed());

    // ... and from here it behaves normally.
    r.run(true, 300);
    r.run(false, 200);
    CHECK_EQ(r.presses, 1);
    CHECK_EQ(r.holds, 0);
}

void test_bounce_is_swallowed() {
    Runner r;
    r.run(false, 200);
    // A dirty contact: 5 ms on, 5 ms off, for 40 ms, then solidly down.
    for (int i = 0; i < 4; ++i) {
        r.run(true, 5);
        r.run(false, 5);
    }
    r.run(true, 300);
    r.run(false, 200);
    // Exactly one press, not five.
    CHECK_EQ(r.presses, 1);
    CHECK_EQ(r.holds, 0);
}

void test_a_blip_shorter_than_the_debounce_produces_nothing() {
    Runner r;
    r.run(false, 200);
    r.run(true, 20);            // 20 ms against a 40 ms debounce
    r.run(false, 300);
    CHECK_EQ(r.presses, 0);
    CHECK_EQ(r.holds, 0);
}

void test_two_presses_are_two_executes() {
    Runner r;
    r.run(false, 200);
    for (int i = 0; i < 3; ++i) {
        r.run(true, 200);
        r.run(false, 200);
    }
    CHECK_EQ(r.presses, 3);
    CHECK_EQ(r.holds, 0);
}

// The task polls at 20 ms; the recogniser must not depend on the 10 ms cadence
// the other cases use.
void test_the_poll_cadence_does_not_change_the_answer() {
    for (const int step : {5, 10, 20, 50}) {
        Runner r;
        r.step_ms = step;
        r.run(false, 200);
        r.run(true, 400);
        r.run(false, 200);
        if (r.presses != 1 || r.holds != 0) {
            std::printf("  step %d ms gave %d presses / %d holds\n", step, r.presses, r.holds);
        }
        CHECK_EQ(r.presses, 1);
        CHECK_EQ(r.holds, 0);

        Runner h;
        h.step_ms = step;
        h.run(false, 200);
        h.run(true, 2600);
        h.run(false, 200);
        CHECK_EQ(h.holds, 1);
        CHECK_EQ(h.presses, 0);
    }
}

void test_the_hold_threshold_is_measured_from_the_debounced_press() {
    // A press recognised at t=40 ms must hold until t=2040 ms, not t=2000 ms:
    // the threshold is measured from when the press became real, so a bouncy
    // switch cannot shorten a deliberate hold.
    GestureConfig cfg;
    cfg.debounce_ms = 40;
    cfg.hold_ms = 2000;
    Runner r(cfg);
    r.step_ms = 10;
    r.run(false, 100);
    r.run(true, 2030);          // 2030 ms down: 40 debounce + 1990 held
    CHECK_EQ(r.holds, 0);       // not yet
    r.run(true, 40);            // now past it
    CHECK_EQ(r.holds, 1);
}

}  // namespace

void run_tests() {
    test_a_short_press_is_one_execute();
    test_a_long_press_is_one_rehome_and_no_execute();
    test_a_very_long_press_still_fires_once();
    test_a_button_held_at_startup_is_ignored_until_released();
    test_bounce_is_swallowed();
    test_a_blip_shorter_than_the_debounce_produces_nothing();
    test_two_presses_are_two_executes();
    test_the_poll_cadence_does_not_change_the_answer();
    test_the_hold_threshold_is_measured_from_the_debounced_press();
}
