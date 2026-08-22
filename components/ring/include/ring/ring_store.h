// Target-side ring storage: mounts LittleFS and loads /fs/ring.json into the
// process-wide RingSet, falling back to the compiled table on any failure
// (spec 4).
//
// THREADING CONTRACT (phase 3 obligation, decision log): reload() mutates the
// live RingSet in place with no lock, and every renderer holds a reference to
// it.  It is safe ONLY at boot (before the modes task starts) or when invoked
// FROM the modes task's own context - the upload handler must dispatch the
// reload through the command path, never call it from the httpd task.
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
