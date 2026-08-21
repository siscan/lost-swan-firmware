// Status LED.  WS2812 (colour-coded) on the DevKitC-1, single active-low LED
// (blink patterns) on the XIAO - spec 2 "Status".
#pragma once

namespace swan {

enum class Status {
    Boot,      // powered, not yet homed
    Homing,
    Ok,        // homed and idle/running
    NoTime,    // homed, SNTP not yet valid (Phase 2)
    Fault,     // one or more columns in FAULT
};

void status_led_init();
void status_led_set(Status s);
void status_led_tick();  // call at 50 Hz from a low-priority task, never from the
                         // motion path - RMT writes do not belong near the ISR

}  // namespace swan
