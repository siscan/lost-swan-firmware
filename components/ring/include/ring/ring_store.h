// Target-side ring storage: mounts LittleFS and loads /fs/ring.json into the
// process-wide RingSet, falling back to the compiled table on any failure
// (spec 4).  The web UI's Settings -> Ring upload (Phase 3) rewrites the file
// and calls reload().
#pragma once

#include "esp_err.h"
#include "ring/ring_runtime.h"

namespace swan {
namespace ring_store {

// Mounts the "storage" LittleFS partition (formatting a virgin one) and loads
// the ring.  Never fails the boot: worst case the compiled table is active.
esp_err_t init();

// Re-reads /fs/ring.json (after an upload).  Returns ESP_OK when the JSON
// table is now active, an error when it fell back.
esp_err_t reload();

const RingSet& get();

}  // namespace ring_store
}  // namespace swan
