// Captive-portal provisioning (spec 10.1) - the IDF shell.
//
// Runs ONLY when there are no credentials, or when somebody explicitly asks.
// Never because the network went away: a router rebooting must not put a
// display in a wall into AP mode.
//
// Policy is pure and lives in components/webapi/portal.h.
#pragma once

#include <string>

#include "esp_err.h"

namespace swan {
namespace net {

// Bring up SoftAP `LOST-Swan-xxxx` (open, so a phone can join without a
// password nobody has been told) and start the DNS responder.  The existing
// http server serves the portal - a second one would need sockets the budget
// does not have.
esp_err_t provision_start();
esp_err_t provision_stop();

bool provisioning();
std::string provision_ssid();

// True while a request should be answered by the portal rather than by the
// normal UI.
bool portal_active();

}  // namespace net
}  // namespace swan
