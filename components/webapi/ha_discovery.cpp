#include "webapi/ha_discovery.h"

#include <array>

#include "ring/json_write.h"
#include "webapi/mqtt_bridge.h"

namespace swan {
namespace api {
namespace {

// The abbreviated keys are HA's own; the long forms work too but roughly
// double the byte count, and the whole set has to fit through a 130 KB heap.
//
//   uniq_id  unique_id      stat_t  state_topic     cmd_t   command_topic
//   avty_t   availability   val_tpl value_template  ent_cat entity_category
//   dev_cla  device_class   stat_cla state_class    o       origin
struct Entity {
    const char* component;   // select | text | button | number | switch | sensor | ...
    const char* object_id;   // unique within the device
    const char* name;
    const char* command;     // <base>cmd/<command>, or nullptr for read-only
    const char* value_tpl;   // against the retained state document
    const char* extra;       // component-specific keys, already JSON
    bool diagnostic;
};

// Everything spec 10.3 lists, plus what phases 3.5 and 4 added and a human
// would want to see: whether the display is simulated, whether it is under
// maintenance, and whether the transport is dropping.
//
// Deliberately NOT here: per-column mode selects.  real/sim/disabled is
// build-out and repair tooling, and putting five three-way selects on a
// dashboard invites somebody to leave a column simulated by accident - the one
// state the whole of 5.10 exists to make unmistakable.  It stays on the
// console and the Settings page, both of which say loudly what they did.
constexpr std::array<Entity, 19> ENTITIES = {{
    {"select", "mode", "Mode", "mode.set", "{{ value_json.mode }}",
     R"("ops":["clock","message","countdown"])", false},

    {"text", "message", "Message", "message.set", nullptr,
     R"("cmd_tpl":"{{ {'tokens': value.split(' ')} | to_json }}","max":40)", false},

    {"button", "execute", "Execute the Numbers", "countdown.execute", nullptr,
     R"("pl_prs":"4 8 15 16 23 42","ic":"mdi:numeric")", false},
    {"button", "cancel", "Cancel countdown", "countdown.cancel", nullptr,
     R"("pl_prs":"","ic":"mdi:timer-off")", false},
    {"button", "rehome", "Re-home all columns", "motion.rehome", nullptr,
     R"("pl_prs":"","ic":"mdi:home-search")", true},

    {"number", "volume", "Volume", "audio.volume", "{{ value_json.audio.volume }}",
     R"("min":0,"max":100,"step":1,"ic":"mdi:volume-high")", false},
    {"switch", "mute", "Mute", "audio.mute", "{{ value_json.audio.mute }}",
     R"("pl_on":"true","pl_off":"false","stat_on":true,"stat_off":false,"ic":"mdi:volume-off")",
     false},
    {"switch", "h24", "24-hour clock", "clock.format", "{{ value_json.cfg.h24 }}",
     R"("pl_on":"true","pl_off":"false","stat_on":true,"stat_off":false)", false},

    {"sensor", "state", "Countdown state", nullptr, "{{ value_json.cd.phase }}",
     R"("ic":"mdi:timer")", false},
    {"sensor", "remaining", "Remaining", nullptr, "{{ value_json.cd.remaining_s }}",
     R"("unit_of_meas":"s","dev_cla":"duration","stat_cla":"measurement")", false},
    // The deadline itself, so an automation can act on WHEN rather than on a
    // number that only means something at the instant it was published.
    {"sensor", "deadline", "Deadline", nullptr,
     "{% if value_json.cd.target > 0 %}"
     "{{ (value_json.cd.target | int) | timestamp_utc }}{% else %}unknown{% endif %}",
     R"("dev_cla":"timestamp")", false},

    {"binary_sensor", "fault", "Column fault", nullptr,
     "{{ 'ON' if value_json.cols | selectattr('state','eq','FAULT') | list | count > 0 "
     "else 'OFF' }}",
     R"("dev_cla":"problem")", false},
    {"binary_sensor", "time_valid", "Time synced", nullptr,
     "{{ 'ON' if value_json.time_valid else 'OFF' }}",
     R"("dev_cla":"connectivity")", true},   // ent_cat comes from the flag
    // Loud on purpose, and a problem class rather than a plain switch: a
    // simulated display looks completely normal, which is the whole hazard.
    {"binary_sensor", "simulated", "Simulated motion", nullptr,
     "{{ 'ON' if value_json.motion.simulated else 'OFF' }}",
     R"("dev_cla":"problem","ic":"mdi:test-tube")", true},
    {"binary_sensor", "maintenance", "Maintenance mode", nullptr,
     "{{ 'ON' if value_json.motion.maintenance else 'OFF' }}",
     R"("dev_cla":"problem","ic":"mdi:wrench")", true},

    {"sensor", "rssi", "WiFi signal", nullptr, "{{ value_json.sys.rssi }}",
     R"("unit_of_meas":"dBm","dev_cla":"signal_strength","stat_cla":"measurement")", true},
    {"sensor", "heap", "Free heap", nullptr, "{{ value_json.sys.heap }}",
     R"("unit_of_meas":"B","stat_cla":"measurement")", true},
    {"sensor", "uptime", "Uptime", nullptr, "{{ value_json.sys.uptime_s }}",
     R"("unit_of_meas":"s","dev_cla":"duration","stat_cla":"total_increasing")", true},
    // Non-zero means the transport could not push everything it wanted to.
    // Silence is what hid the stale-mirror bug; this is the same lesson.
    {"sensor", "dropped", "Messages dropped", nullptr,
     "{{ value_json.sys.ws_dropped + value_json.mqtt.dropped }}",
     R"("stat_cla":"total_increasing","ic":"mdi:message-alert")", true},
}};

void write_device(json::Writer& w, const HaContext& ctx) {
    w.key("dev").obj()
        .kv("ids", "swan_" + ctx.device_id)
        .kv("name", "LOST Swan Timer")
        .kv("mf", "DHARMA Initiative")
        .kv("mdl", "Station 3 split-flap")
        .kv("sw", ctx.version)
        .end_obj();
    w.key("o").obj()
        .kv("name", "lost-swan-firmware")
        .kv("sw", ctx.version)
        .kv("url", "https://github.com/siscan/lost-swan-firmware")
        .end_obj();
}

}  // namespace

std::size_t ha_entity_count() { return ENTITIES.size(); }

bool ha_entity(std::size_t i, const HaContext& ctx, bool announce, std::string& topic,
               std::string& payload) {
    topic.clear();
    payload.clear();
    if (i >= ENTITIES.size()) return false;
    const Entity& e = ENTITIES[i];

    topic = ctx.prefix + "/" + e.component + "/swan_" + ctx.device_id + "/" + e.object_id +
            "/config";
    // An empty retained payload deletes the retained message, which is the only
    // way to remove the device from Home Assistant.  Leaving it out is how you
    // get a ghost that is permanently "unavailable".
    if (!announce) return true;

    json::Writer w;
    w.obj();
    w.kv("name", e.name);
    w.kv("uniq_id", "swan_" + ctx.device_id + "_" + e.object_id);
    w.kv("stat_t", ctx.base + TOPIC_STATE);
    if (e.value_tpl != nullptr) w.kv("val_tpl", e.value_tpl);
    if (e.command != nullptr) w.kv("cmd_t", ctx.base + "cmd/" + e.command);
    // Availability rides the same retained topic the LWT writes, so an entity
    // greys out when the display loses power rather than showing its last
    // value for ever.
    w.kv("avty_t", ctx.base + TOPIC_AVAILABILITY);
    w.kv("pl_avail", PAYLOAD_ONLINE);
    w.kv("pl_not_avail", PAYLOAD_OFFLINE);
    if (e.diagnostic) w.kv("ent_cat", "diagnostic");
    if (e.extra != nullptr) w.raw(e.extra);   // a whole "key":value pair; raw() supplies the comma
    write_device(w, ctx);
    w.end_obj();
    payload = w.take();
    return true;
}

}  // namespace api
}  // namespace swan
