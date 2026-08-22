// Target-side time service: SNTP via esp_sntp (spec 8).  WiFi itself is
// Phase 4; until credentials exist SNTP simply never syncs and the clock
// shows the WiFi glyph per spec 7.1.
#pragma once

#include "esp_err.h"
#include "timesvc/time_source.h"

namespace swan {
namespace time_service {

// Starts SNTP with the configured server list.  Idempotent.
esp_err_t init(const char* ntp_server);

// The process-wide TimeSource backed by the system clock + SNTP sync flag.
TimeSource& source();

}  // namespace time_service
}  // namespace swan
