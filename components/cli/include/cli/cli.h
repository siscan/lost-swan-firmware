// Serial console for bring-up.  Spec 13.
#pragma once

#include "esp_err.h"

namespace swan {
namespace cli {

// Registers the Phase 1 command set and starts the REPL on USB-Serial-JTAG.
esp_err_t start();

}  // namespace cli
}  // namespace swan
