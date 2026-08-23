// Home Assistant MQTT discovery (spec 10.3).  Pure - host-tested.
//
// One device, "LOST Swan Timer", with every entity pointing at the retained
// `<base>state` document and driven through `<base>cmd/<command>`.  There are
// no HA-only commands: an HA button publishes exactly what a human would type
// at mosquitto_pub, which is what keeps MQTT a transport rather than a second
// command set.
//
// Documents are generated ONE AT A TIME.  The whole set is ~14 KB and the
// device has ~130 KB of heap with WiFi and a web server in it; building them
// all into one buffer to "publish the discovery set" is how a feature that
// works on the bench fails on a busy display.
#pragma once

#include <cstddef>
#include <string>

namespace swan {
namespace api {

// What the documents need that is not a constant.
struct HaContext {
    std::string base = "swan/";        // normalised, with the trailing slash
    std::string prefix = "homeassistant";  // HA's own discovery_prefix
    std::string device_id = "swan";    // from the base MAC, so a reflash keeps
                                       // HA's entity ids and their history
    std::string version = "dev";       // esp_app_desc, for the device block
    int columns = 5;
};

// How many entities the display advertises.
std::size_t ha_entity_count();

// Build entity `i`.  Returns false if the index is out of range.
//
// `payload` is empty when `announce` is false: an empty RETAINED payload is how
// MQTT deletes a retained message, and it is the only way to remove a device
// from Home Assistant.  Without it, turning MQTT off leaves a permanently
// unavailable ghost device behind for ever.
bool ha_entity(std::size_t i, const HaContext& ctx, bool announce, std::string& topic,
               std::string& payload);

}  // namespace api
}  // namespace swan
