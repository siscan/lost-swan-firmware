// WiFi STA + mDNS (spec 10.1), pulled forward into Phase 3 so the web UI is
// reachable the day the board arrives.
//
// Credentials come from NVS (`wifi <ssid> <pass>` on the console).  Captive
// portal provisioning, MQTT and OTA are Phase 4; nothing here waits on any of
// them, and nothing on the motion path touches this.
#pragma once

#include <cstdint>
#include <string>

#include "esp_err.h"

namespace swan {
namespace net {

enum class WifiState : unsigned char {
    Disabled,    // no credentials stored - the display runs standalone
    Connecting,
    Connected,
    Failed,      // retrying with backoff
};

const char* wifi_state_name(WifiState s);

struct WifiStatus {
    WifiState state = WifiState::Disabled;
    std::string ssid;
    std::string ip;
    int rssi = 0;
    uint32_t disconnects = 0;
};

// Brings up the netif/event loop and, if credentials exist, starts the STA.
// Never blocks the boot and never fails it: with no credentials the display is
// a standalone clock that has not synced (spec 7.1 shows the WiFi glyph).
esp_err_t init();

// Stores credentials and (re)connects.  Empty ssid clears them and stops.
esp_err_t set_credentials(const std::string& ssid, const std::string& pass);

WifiStatus status();

// Told when the STA gains or loses an IP.
//
// on_got_ip used to write the status struct and tell nobody, so a second
// subsystem had no way to learn it could open a socket short of polling.  MQTT
// must not start before there is a route: an unreachable broker with no link
// logs a connection error every few seconds for ever.
//
// Runs on the system event task - do not block, do not call back into net::.
using LinkCallback = void (*)(bool up);
void on_link_change(LinkCallback cb);

// Starts mDNS so the UI is at http://lost.local/ (spec 10.1).  Safe to call
// before the link is up.
esp_err_t mdns_start(const char* hostname, const char* instance);

}  // namespace net
}  // namespace swan
