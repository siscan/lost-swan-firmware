// Target-side ring storage: mounts LittleFS and loads /fs/ring.json into the
// process-wide RingSet, falling back to the compiled table on any failure
// (spec 4).
//
// THREADING CONTRACT.  reload() and the upload swap mutate the live RingSet in
// place, replacing shared_ptrs and thereby FREEING the outgoing tables.  Two
// rules follow, and the second is the one the phase 3 review found missing:
//
//  1. WRITERS: only at boot (before the modes task starts) or from the modes
//     task's own context, and the upload swap must go through
//     ModeManager::cmd_ring_swap so it holds the same lock every command
//     takes.  Never from the httpd task.
//
//  2. READERS OUTSIDE THE MODES TASK - the httpd task, the CLI - must not hold
//     a `const RingTable&` or a `const RingSet&` across anything.  They take a
//     pinned copy (api::RingSource::snapshot(), implemented by RingStager),
//     which is cheap and keeps the tables alive for as long as the copy lives.
//     Reading through get() from another task is a use-after-free waiting for
//     an upload: the swap drops the last reference mid-read.  This was a live
//     bug - /api/ring, /api/state, /api/wear and three CLI commands all did
//     it, and only the host dev server's coarse lock hid it.
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
