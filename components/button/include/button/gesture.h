// The physical button's gesture recogniser (spec §2 Q6): debounce, press
// versus hold, and the one safety rule the hardware forces on us.
//
// Pure - no IDF includes - so all three behaviours are host-tested rather than
// discovered on a wall-mounted display.  The shell (button.cpp) does nothing
// but read the pin, feed this, and dispatch.
//
// THE HARDWARE FACT THIS EXISTS FOR.  The button is wired in parallel with the
// onboard BOOT button on GPIO28, which is an ESP32-C5 **strapping pin**: the
// ROM samples it at reset and a LOW level selects the serial bootloader.  No
// firmware can change that - the ROM has already decided before a single
// instruction of ours runs.  What firmware CAN do is refuse to compound it:
// a button that is already down when we start is IGNORED until it is released.
// Otherwise a person who holds the button across a reset - the exact gesture
// that lands some boards in the bootloader - gets an unexplained EXECUTE or
// REHOME on the boot that does come up.  See `armed()`.
#pragma once

#include <cstdint>

namespace swan {
namespace button {

enum class Gesture : unsigned char {
    None,
    Press,   // a short press, released before the hold threshold
    Hold,    // held past the threshold; fires ONCE, while still held
};

struct GestureConfig {
    // Long enough to swallow contact bounce, short enough that a deliberate
    // tap is never lost.  A tactile switch settles well inside 10 ms; 40 gives
    // margin for a long loom to a panel-mounted button.
    uint32_t debounce_ms = 40;
    // Spec Q6: "press = Execute, hold = rehome".  Two seconds is long enough
    // that nobody re-homes the display by accident and short enough to feel
    // deliberate rather than broken.
    uint32_t hold_ms = 2000;
};

class ButtonGesture {
public:
    ButtonGesture() = default;
    explicit ButtonGesture(GestureConfig cfg) : cfg_(cfg) {}

    // `pressed` is the LOGICAL level (the shell inverts the active-low pin).
    // Call at a steady cadence; `now_ms` may be any monotonic millisecond
    // clock.  Returns at most one gesture per call.
    Gesture feed(bool pressed, int64_t now_ms);

    // False until the button has been seen released at least once.  A button
    // held from before this object existed - across a reset, or a stuck switch
    // - produces nothing at all until it is let go.
    bool armed() const { return armed_; }

    // True while a debounced press is in progress.  For reporting only.
    bool down() const { return armed_ && stable_; }

private:
    GestureConfig cfg_{};
    bool armed_ = false;        // has the button been released since we started
    bool stable_ = false;       // the debounced level
    bool raw_ = false;          // the last raw sample
    bool hold_fired_ = false;   // this press has already produced a Hold
    int64_t changed_at_ = 0;    // when raw_ last changed
    int64_t pressed_at_ = 0;    // when stable_ became true
    bool have_sample_ = false;
};

}  // namespace button
}  // namespace swan
