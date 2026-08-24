// OTA with rollback (spec 10.4) - the IDF shell.  All policy is pure and lives
// in components/webapi/ota_policy.h.
#pragma once

#include <cstdint>
#include <string>

#include "esp_err.h"
#include "webapi/api.h"
#include "webapi/ota_policy.h"

namespace swan {
namespace net {

// Starts the confirm watcher.  Short-circuits immediately unless the running
// image is PENDING_VERIFY, so it costs nothing on an ordinary boot.
// Takes no Context: it must be startable as the FIRST thing app_main does,
// before the Context exists, because an image that hangs before this point
// would otherwise never be able to roll itself back.
esp_err_t ota_init();

// The upload route needs the dispatcher, for the motion hold.
void ota_bind(api::Context& ctx);

struct OtaState {
    bool pending_verify = false;   // this image has not confirmed itself yet
    bool in_progress = false;
    uint32_t received = 0;
    std::string running_partition;
    std::string last_error;
    std::string boot_verdict = "n/a";
};
OtaState ota_status();

// Confirm or roll back by hand, so a human is never stuck watching a timer.
esp_err_t ota_confirm();
esp_err_t ota_rollback_and_reboot();

// What the running image is, for the gate.
std::string ota_running_project();
std::string ota_running_board();
bool ota_pending_verify();

// Registered by httpd_start.
esp_err_t ota_register_routes(void* server);

}  // namespace net
}  // namespace swan
