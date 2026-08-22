// Serial console for bring-up.  Spec 13.
#pragma once

#include <cstdint>

#include "esp_err.h"

namespace swan {

class ModeManager;

namespace cli {

// Wires the mode/frame/countdown commands to the dispatcher.  Call before
// start(); without it those commands report "modes not available" (the motion
// bring-up set works regardless).
void bind_modes(ModeManager* mm, int64_t (*utc_ms_fn)());

// Registers the command set and starts the REPL on USB-Serial-JTAG.
esp_err_t start();

}  // namespace cli
}  // namespace swan
