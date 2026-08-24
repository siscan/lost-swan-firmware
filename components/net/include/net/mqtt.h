// The MQTT transport (spec 10.2a, 10.3) - the IDF shell.
//
// MQTT is the canonical EXTERNAL API: a separate Swan terminal prop drives the
// display with it.  It is also entirely optional - off until configured, never
// waited on, and the display is a complete standalone clock without it
// (spec 10.0).
//
// All policy is pure and lives in components/webapi/mqtt_bridge.h; this file
// is the client, the tasks and the queues.  Nothing here decides what a
// command means: every inbound message becomes the same document the web UI
// posts and goes to the same api::handle_command.
#pragma once

#include <cstdint>
#include <string>

#include "webapi/mqtt_bridge.h"

#include "esp_err.h"
#include "webapi/api.h"

namespace swan {
namespace net {

// Starts the transport task.  Safe to call with MQTT disabled - nothing is
// allocated and no client exists until a link comes up with a valid config.
esp_err_t mqtt_init(api::Context& ctx);

// Apply a new configuration.  Staged and performed on the MQTT task, never on
// the caller's: esp_mqtt_client_stop refuses to run on the client's own task
// and waits with portMAX_DELAY while that task may be parked for seconds, so
// calling it from the HTTP task would stall the whole web UI.
esp_err_t mqtt_reconfigure();

// Queue a message for publication.  NON-BLOCKING: pushes into our own bounded
// ring and returns.  Producers - the modes task, the HTTP task - must never
// touch esp_mqtt_* themselves, because esp_mqtt_client_publish blocks the
// CALLER for up to ten seconds on a network timeout and the modes task owns a
// 20 Hz tick.
void mqtt_publish(const std::string& topic_leaf, const std::string& payload, bool retain,
                  int qos);

// Publish the retained "offline" note and stop cleanly, waiting bounded for it
// to reach the broker.
//
// The Last Will covers an ungraceful drop only: esp_mqtt_client_stop sends a
// clean DISCONNECT, and the broker then DISCARDS the will (MQTT 3.1.1 3.14.4).
// Without this, a reboot from the UI leaves the display looking online to
// Home Assistant until the keepalive expires.
void mqtt_go_offline();

// The terminal prop's presence as last heard on swan/prop/terminal.  Read-only
// - this display never publishes that topic.  Returns a copy under the
// transport lock; `seen` is false until a document has arrived this session.
api::PropPresence mqtt_prop();

bool mqtt_connected();
bool mqtt_enabled();
uint32_t mqtt_dropped();

}  // namespace net
}  // namespace swan
