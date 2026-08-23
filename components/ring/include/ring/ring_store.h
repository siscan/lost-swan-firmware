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

#include <string>

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

// The same object, writable.  ONLY the ring upload stager takes this, and it
// only writes from the modes task - the contract above, spelled out in a type.
RingSet& mutable_ring();

// Persists an ALREADY-VALIDATED ring.json to /fs/ring.json.  Called from the
// modes task right after the staged table went live, so the file on flash and
// the table in RAM can never disagree, and a rejected upload never reaches the
// filesystem at all.  Writes to a temp file and renames, so a power cut during
// the write leaves the previous ring.json intact.
esp_err_t write_accepted(const std::string& body);

}  // namespace ring_store
}  // namespace swan
