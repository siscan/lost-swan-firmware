// Home Assistant discovery (spec 10.3).
//
// The properties that matter are structural, and all of them are checkable
// without Home Assistant: every document is valid JSON, every unique id is
// unique, every command topic names a command the dispatcher actually has,
// every entity is retractable, and the whole set fits in the heap this device
// has.
#include <set>
#include <string>

#include "check.h"
#include "ring/json_lite.h"
#include "webapi/ha_discovery.h"
#include "webapi/mqtt_bridge.h"

using namespace swan;
using namespace swan::api;

namespace {

HaContext ctx() {
    HaContext c;
    c.base = normalize_base("swan");
    c.prefix = "homeassistant";
    c.device_id = "10bda3dda8e8";
    c.version = "0.4.0+devkitc1.sim";
    return c;
}

// Every command name the dispatcher implements that discovery is allowed to
// reference.  Mirrors spec 10.2a; a typo in a cmd_t would otherwise produce an
// HA button that silently does nothing.
const std::set<std::string>& known_commands() {
    static const std::set<std::string> k = {
        "mode.set",          "message.set",         "countdown.execute", "countdown.start",
        "countdown.reset",   "countdown.cancel",    "countdown.set_target",
        "preset.set",        "display.frame",       "audio.volume",      "audio.mute",
        "audio.play",        "motion.rehome",       "motion.column",     "motion.maintenance",
        "motion.sim_fault",  "motion.cal",          "motion.spin",       "clock.format",
        "config.set",        "config.save",         "mqtt.config",       "system.reboot",
    };
    return k;
}

void test_every_document_is_valid_and_unique() {
    const HaContext c = ctx();
    std::set<std::string> topics, uniq_ids;
    std::size_t total_bytes = 0;

    CHECK(ha_entity_count() > 0);
    for (std::size_t i = 0; i < ha_entity_count(); ++i) {
        std::string topic, payload;
        CHECK(ha_entity(i, c, true, topic, payload));
        total_bytes += topic.size() + payload.size();

        // A duplicate topic means one entity silently overwrites another.
        CHECK(topics.insert(topic).second);
        CHECK(topic.rfind("homeassistant/", 0) == 0);
        CHECK(topic.size() > 14 && topic.substr(topic.size() - 7) == "/config");

        json::Value v;
        CHECK(json::parse(payload, v, nullptr));
        CHECK(v.type == json::Type::Object);

        // A duplicate unique id makes HA drop the second entity without saying
        // so, which is exactly the kind of thing nobody notices for months.
        const json::Value* u = v.get("uniq_id");
        CHECK(u != nullptr);
        CHECK(uniq_ids.insert(std::string(u->as_str())).second);

        // Availability on EVERY entity: without it an entity keeps showing its
        // last value after the display loses power.
        CHECK(v.get("avty_t") != nullptr);
        CHECK(v.get("avty_t")->as_str() == "swan/availability");
        CHECK(v.get("pl_avail")->as_str() == PAYLOAD_ONLINE);
        CHECK(v.get("pl_not_avail")->as_str() == PAYLOAD_OFFLINE);

        // One device, so the entities group rather than littering the UI.
        const json::Value* dev = v.get("dev");
        CHECK(dev != nullptr && dev->get("ids") != nullptr);
        CHECK(dev->get("ids")->as_str() == "swan_10bda3dda8e8");
        CHECK(dev->get("sw")->as_str() == "0.4.0+devkitc1.sim");

        // A command topic must name a command that exists, and must be under
        // our base - discovery is not a place to invent a second command set.
        const json::Value* cmd = v.get("cmd_t");
        if (cmd != nullptr) {
            const std::string t(cmd->as_str());
            CHECK(t.rfind("swan/cmd/", 0) == 0);
            const std::string name = t.substr(9);
            CHECK(known_commands().count(name) == 1);
        }
        // Anything that is neither readable nor writable is dead weight.
        CHECK(v.get("val_tpl") != nullptr || cmd != nullptr);
    }

    // Sized against the device, not against a desktop.  The board reports
    // ~130 KB free with WiFi and httpd up, and the documents are published one
    // at a time - but if the SET ever approached the heap, publishing it would
    // become a memory event rather than a network one.
    CHECK(total_bytes < 24000);
    std::printf("  discovery set: %u entities, %u bytes total\n",
                static_cast<unsigned>(ha_entity_count()), static_cast<unsigned>(total_bytes));
}

// Turning MQTT off has to REMOVE the device, not leave a ghost that is
// permanently unavailable.  An empty retained payload is the only way to do
// that, and the retraction set must cover exactly the announcement set.
void test_retraction_covers_the_announcement() {
    const HaContext c = ctx();
    std::set<std::string> announced, retracted;
    for (std::size_t i = 0; i < ha_entity_count(); ++i) {
        std::string t, p;
        CHECK(ha_entity(i, c, true, t, p));
        CHECK(!p.empty());
        announced.insert(t);

        std::string t2, p2;
        CHECK(ha_entity(i, c, false, t2, p2));
        CHECK(p2.empty());          // empty payload = delete the retained message
        retracted.insert(t2);
    }
    CHECK(announced == retracted);
}

void test_prefix_and_base_are_honoured() {
    HaContext c = ctx();
    c.prefix = "ha";                      // HA's discovery_prefix is configurable
    c.base = normalize_base("home/swan");
    std::string t, p;
    CHECK(ha_entity(0, c, true, t, p));
    CHECK(t.rfind("ha/", 0) == 0);
    json::Value v;
    CHECK(json::parse(p, v, nullptr));
    CHECK(v.get("stat_t")->as_str() == "home/swan/state");
    CHECK(v.get("avty_t")->as_str() == "home/swan/availability");
    CHECK(v.get("cmd_t")->as_str() == "home/swan/cmd/mode.set");

    // The device id comes from the MAC so a reflash keeps HA's entity history.
    c.device_id = "aabbccddeeff";
    CHECK(ha_entity(0, c, true, t, p));
    CHECK(t.find("swan_aabbccddeeff") != std::string::npos);
}

void test_out_of_range() {
    const HaContext c = ctx();
    std::string t, p;
    CHECK(!ha_entity(ha_entity_count(), c, true, t, p));
    CHECK(!ha_entity(ha_entity_count() + 100, c, true, t, p));
    CHECK(t.empty() && p.empty());
}

// The display must be impossible to mistake for a real one, on every surface
// (spec 5.10) - Home Assistant included.
void test_the_simulation_is_advertised() {
    const HaContext c = ctx();
    bool found_sim = false, found_maint = false;
    for (std::size_t i = 0; i < ha_entity_count(); ++i) {
        std::string t, p;
        ha_entity(i, c, true, t, p);
        if (t.find("/simulated/") != std::string::npos) {
            found_sim = true;
            json::Value v;
            CHECK(json::parse(p, v, nullptr));
            // A "problem" class, not a plain switch: it should look like
            // something wrong, because a simulated display in a hallway is.
            CHECK(v.get("dev_cla")->as_str() == "problem");
            CHECK(std::string(v.get("val_tpl")->as_str()).find("motion.simulated") !=
                  std::string::npos);
        }
        if (t.find("/maintenance/") != std::string::npos) found_maint = true;
    }
    CHECK(found_sim);
    CHECK(found_maint);
}

}  // namespace

void run_tests() {
    test_every_document_is_valid_and_unique();
    test_retraction_covers_the_announcement();
    test_prefix_and_base_are_honoured();
    test_out_of_range();
    test_the_simulation_is_advertised();
}
