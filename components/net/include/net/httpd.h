// The device's web server (spec 10.2): static UI from LittleFS, WebSocket /ws
// with state push plus a 1 Hz heartbeat, and REST under /api/.
//
// Every route maps onto webapi's single dispatcher (spec 10.2a) - there is no
// command path here, only transport.  The HTTP task therefore never reaches
// mode state except through ModeManager's mutex, which test_api asserts.
#pragma once

#include <cstddef>
#include <string>

#include "esp_err.h"
#include "webapi/api.h"

namespace swan {
namespace net {

// `ctx` must outlive the server (it does: main owns it for the device's life).
esp_err_t httpd_start(api::Context& ctx);
esp_err_t httpd_stop();

// Push one message to every open /ws client.  Safe from any task; sends are
// queued onto the server's own task, so a slow client cannot stall the caller.
void ws_broadcast(const std::string& msg);

size_t ws_clients();

}  // namespace net
}  // namespace swan
