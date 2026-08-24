#include "button/gesture.h"

namespace swan {
namespace button {

Gesture ButtonGesture::feed(bool pressed, int64_t now_ms) {
    if (!have_sample_) {
        // The very first sample establishes the level without producing a
        // transition.  If the button is already down here it was down before we
        // existed - see the header - so we stay disarmed until it is released.
        have_sample_ = true;
        raw_ = pressed;
        stable_ = pressed;
        changed_at_ = now_ms;
        pressed_at_ = now_ms;
        armed_ = !pressed;
        return Gesture::None;
    }

    if (pressed != raw_) {
        raw_ = pressed;
        changed_at_ = now_ms;
        return Gesture::None;   // wait for it to settle
    }

    // The raw level has been steady since changed_at_.
    const bool settled = (now_ms - changed_at_) >= static_cast<int64_t>(cfg_.debounce_ms);
    if (settled && raw_ != stable_) {
        stable_ = raw_;
        if (stable_) {
            pressed_at_ = now_ms;
            hold_fired_ = false;
        } else {
            // Released.  This is what arms a button that was down at startup,
            // and it is also where a short press is reported - on release, so
            // that a press which turns into a hold produces only the hold.
            const bool was_armed = armed_;
            armed_ = true;
            if (was_armed && !hold_fired_) return Gesture::Press;
            return Gesture::None;
        }
    }

    if (armed_ && stable_ && !hold_fired_ &&
        (now_ms - pressed_at_) >= static_cast<int64_t>(cfg_.hold_ms)) {
        hold_fired_ = true;
        // Fired while the button is STILL DOWN, deliberately: a re-home you
        // have to let go of to start is a re-home you cannot tell you have
        // started.  The display begins moving under your thumb, which is the
        // feedback.
        return Gesture::Hold;
    }

    return Gesture::None;
}

}  // namespace button
}  // namespace swan
