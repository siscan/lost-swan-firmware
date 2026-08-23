// The MQTT transport's pure half (spec 10.2a, 10.3).
//
// The point of this suite is the rule: MQTT adds a transport, not a command
// set.  Everything a broker delivers has to end up as the same document the
// web UI posts, validated by the same dispatcher - so the parsing that turns a
// topic and a payload into that document is where a second command set would
// sneak in, and it is tested here without needing a broker.
#include <string>
#include <string_view>

#include "check.h"
#include "ring/json_lite.h"
#include "webapi/mqtt_bridge.h"

using namespace swan;
using namespace swan::api;

namespace {

// esp-mqtt hands us pointers into its receive buffer, NOT null-terminated
// strings.  Building the views over a larger backing buffer here means a stray
// strlen or a missing length would read the neighbouring bytes and fail the
// test, rather than passing by luck on a terminator that will not be there.
struct Unterminated {
    std::string backing;
    std::string_view topic;
    std::string_view payload;

    Unterminated(std::string_view t, std::string_view p) {
        backing = std::string(t) + std::string(p) + "GARBAGE-AFTER-THE-BUFFER";
        topic = std::string_view(backing).substr(0, t.size());
        payload = std::string_view(backing).substr(t.size(), p.size());
    }
};

std::string cmd_of(const std::string& body) {
    json::Value v;
    if (!json::parse(body, v, nullptr)) return "<unparseable>";
    return v.get("cmd") ? std::string(v.get("cmd")->as_str()) : "<none>";
}

bool has_payload(const std::string& body) {
    json::Value v;
    return json::parse(body, v, nullptr) && v.get("payload") != nullptr;
}

// --------------------------------------------------------------------------
void test_base_normalisation() {
    CHECK(normalize_base("swan") == "swan/");
    CHECK(normalize_base("swan/") == "swan/");
    CHECK(normalize_base("/swan/") == "swan/");
    CHECK(normalize_base("///swan///") == "swan/");
    CHECK(normalize_base("home/swan") == "home/swan/");
    // A base nobody configured falls back to the spec default rather than
    // producing topics that begin with a slash.
    CHECK(normalize_base("") == "swan/");
    CHECK(normalize_base("/") == "swan/");
    CHECK(normalize_base("////") == "swan/");
}

void test_topic_to_command() {
    std::string body, cmd;
    const std::string base = normalize_base("swan");

    // A bare value, which is what a human types and what spec 10.2a promises.
    {
        Unterminated u("swan/cmd/mode.set", "clock");
        CHECK(command_from_topic(base, u.topic, u.payload, body, cmd) == CmdParse::Ok);
        CHECK_STREQ(cmd.c_str(), "mode.set");
        CHECK_STREQ(body.c_str(), R"({"cmd":"mode.set","payload":"clock"})");
    }
    // A structured payload passes through as JSON, not as a quoted string.
    {
        Unterminated u("swan/cmd/motion.column", R"({"column":2,"mode":"disabled"})");
        CHECK(command_from_topic(base, u.topic, u.payload, body, cmd) == CmdParse::Ok);
        json::Value v;
        CHECK(json::parse(body, v, nullptr));
        const json::Value* p = v.get("payload");
        CHECK(p != nullptr && p->type == json::Type::Object);
        CHECK_EQ(p->get("column")->as_int(-1), 2);
    }
    // No payload at all: countdown.cancel and system.reboot take none, and a
    // synthesised empty string would be a payload the dispatcher then has to
    // ignore.
    {
        Unterminated u("swan/cmd/countdown.cancel", "");
        CHECK(command_from_topic(base, u.topic, u.payload, body, cmd) == CmdParse::Ok);
        CHECK(!has_payload(body));
        CHECK_STREQ(body.c_str(), R"({"cmd":"countdown.cancel"})");
    }
    // A numeric bare payload stays a number, so countdown.set_target works the
    // way the spec's "a bare string is accepted where the payload is a single
    // value" implies.
    {
        Unterminated u("swan/cmd/countdown.set_target", "1787000000");
        CHECK(command_from_topic(base, u.topic, u.payload, body, cmd) == CmdParse::Ok);
        json::Value v;
        CHECK(json::parse(body, v, nullptr));
        CHECK_EQ(v.get("payload")->as_int(0), 1787000000);
    }
    {
        Unterminated u("swan/cmd/clock.format", "true");
        CHECK(command_from_topic(base, u.topic, u.payload, body, cmd) == CmdParse::Ok);
        json::Value v;
        CHECK(json::parse(body, v, nullptr));
        CHECK(v.get("payload")->boolean);
    }
}

// THE rule: no arguments in topic segments, no MQTT-only names.
void test_no_second_command_grammar() {
    std::string body, cmd;
    const std::string base = normalize_base("swan");

    // An argument smuggled into the topic is refused, not half-understood.
    {
        Unterminated u("swan/cmd/motion.column/2", "sim");
        CHECK(command_from_topic(base, u.topic, u.payload, body, cmd) == CmdParse::BadCommand);
        CHECK(body.empty());
    }
    // Anything outside the command charset. A wildcard subscription means the
    // broker can hand us whatever it likes.
    for (const char* t : {"swan/cmd/mode set", "swan/cmd/mode\"set", "swan/cmd/../../etc",
                          "swan/cmd/mode-set", "swan/cmd/#"}) {
        Unterminated u(t, "x");
        CHECK(command_from_topic(base, u.topic, u.payload, body, cmd) == CmdParse::BadCommand);
    }
    // A name long enough to be an attack rather than a typo.
    {
        const std::string longcmd = "swan/cmd/" + std::string(64, 'a');
        Unterminated u(longcmd, "x");
        CHECK(command_from_topic(base, u.topic, u.payload, body, cmd) == CmdParse::BadCommand);
    }
    // Empty command.
    {
        Unterminated u("swan/cmd/", "x");
        CHECK(command_from_topic(base, u.topic, u.payload, body, cmd) == CmdParse::EmptyCommand);
    }
    // Not ours at all - our own published topics come back on a wildcard
    // subscription, and answering them would be a loop.
    for (const char* t : {"swan/state", "swan/event", "swan/countdown", "swan/availability",
                          "swan", "other/cmd/mode.set", "homeassistant/select/swan/mode/config"}) {
        Unterminated u(t, "x");
        CHECK(command_from_topic(base, u.topic, u.payload, body, cmd) == CmdParse::NotOurs);
    }
    // A different base is honoured, and the default base is not special-cased.
    {
        const std::string other = normalize_base("home/swan");
        Unterminated u("home/swan/cmd/mode.set", "clock");
        CHECK(command_from_topic(other, u.topic, u.payload, body, cmd) == CmdParse::Ok);
        CHECK_STREQ(cmd.c_str(), "mode.set");
        Unterminated v("swan/cmd/mode.set", "clock");
        CHECK(command_from_topic(other, v.topic, v.payload, body, cmd) == CmdParse::NotOurs);
    }
}

// A payload is untrusted input from a broker anyone on the LAN can publish to.
// It must never be able to break out of the document being built.
void test_payload_cannot_escape() {
    std::string body, cmd;
    const std::string base = normalize_base("swan");
    for (const char* p : {R"(a"b)", R"(a\b)", "a\nb", "a\tb", R"({"unclosed":)", "\x01\x02"}) {
        Unterminated u("swan/cmd/message.set", p);
        CHECK(command_from_topic(base, u.topic, u.payload, body, cmd) == CmdParse::Ok);
        json::Value v;
        CHECK(json::parse(body, v, nullptr));           // still one valid document
        CHECK(v.get("cmd") != nullptr);
        CHECK_STREQ(std::string(v.get("cmd")->as_str()).c_str(), "message.set");
    }
}

void test_countdown_doc() {
    const std::string d = countdown_doc("running", 1787000000, "mqtt", 7);
    json::Value v;
    CHECK(json::parse(d, v, nullptr));
    CHECK_STREQ(std::string(v.get("state")->as_str()).c_str(), "running");
    CHECK_EQ(v.get("target")->as_int(0), 1787000000);
    CHECK_STREQ(std::string(v.get("set_by")->as_str()).c_str(), "mqtt");
    CHECK_EQ(v.get("seq")->as_int(0), 7);
    // Exactly the four fields spec 7.3 names, in order - a prop parses this
    // and nothing else, so a fifth would be a silent contract change.
    CHECK_EQ(v.members.size(), 4u);
    CHECK_STREQ(v.members[0].first.c_str(), "state");
    CHECK_STREQ(v.members[1].first.c_str(), "target");
    CHECK_STREQ(v.members[2].first.c_str(), "set_by");
    CHECK_STREQ(v.members[3].first.c_str(), "seq");
}

void test_display_slice_ignores_the_countdown_tick() {
    // A running countdown decrements cd.remaining_s every second.  It is
    // derived from cd.target, which is absolute, so republishing a RETAINED
    // topic for it would rewrite the broker's store once a second for 108
    // minutes - measured on the board as 11 state documents in 12 s on a
    // display that was otherwise doing nothing.
    const std::string a =
        R"({"mode":"countdown","cd":{"phase":"running","target":1787527865,"remaining_s":6479},)"
        R"("sys":{"heap":1}})";
    const std::string b =
        R"({"mode":"countdown","cd":{"phase":"running","target":1787527865,"remaining_s":6478},)"
        R"("sys":{"heap":2}})";
    CHECK(display_slice(a) == display_slice(b));

    // ... but the target moving IS a change: somebody set a new deadline.
    const std::string c =
        R"({"mode":"countdown","cd":{"phase":"running","target":1787530000,"remaining_s":6478},)"
        R"("sys":{"heap":1}})";
    CHECK(display_slice(a) != display_slice(c));

    // ... and so is the phase, which is what a peer acts on.
    const std::string d =
        R"({"mode":"countdown","cd":{"phase":"zero","target":1787527865,"remaining_s":0},)"
        R"("sys":{"heap":1}})";
    CHECK(display_slice(a) != display_slice(d));

    // A document that stops carrying the field is still distinguishable from
    // one that carries it, so eliding the value never merges two states.
    const std::string e = R"({"mode":"clock","cd":{"phase":"idle"},"sys":{"heap":1}})";
    const std::string f =
        R"({"mode":"clock","cd":{"phase":"idle","remaining_s":0},"sys":{"heap":1}})";
    CHECK(display_slice(e) != display_slice(f));
}

void test_display_slice() {
    // The slice must exclude sys, whose heap and uptime change every tick: a
    // retained publish rewrites the broker's store and makes HA re-evaluate
    // every template, so "changed" being permanently true costs more here than
    // it did on /ws.
    const std::string a =
        R"({"e":"state","t":1,"mode":"clock","cd":{},"sys":{"heap":141000,"uptime_s":10}})";
    const std::string b =
        R"({"e":"state","t":2,"mode":"clock","cd":{},"sys":{"heap":140012,"uptime_s":11}})";
    const std::string c =
        R"({"e":"state","t":3,"mode":"message","cd":{},"sys":{"heap":141000,"uptime_s":12}})";
    CHECK(display_slice(a) == display_slice(b));   // heap and uptime alone: no publish
    CHECK(display_slice(a) != display_slice(c));   // the mode changed: publish
    CHECK(display_slice(a).find("heap") == std::string_view::npos);
    CHECK(display_slice(a).find("\"mode\":\"clock\"") != std::string_view::npos);
    // A document without the markers is compared whole rather than silently
    // becoming "never changes".
    CHECK(display_slice("{}") == "{}");
}

void test_broker_uri() {
    std::string why;
    CHECK(broker_uri_valid("mqtt://192.168.10.5:1883", why));
    CHECK(broker_uri_valid("mqtt://broker.local", why));
    CHECK(broker_uri_valid("ws://broker.local:9001/mqtt", why));

    // The forms that would otherwise fail deep inside esp-mqtt with an error
    // nobody can act on from a Settings page.
    CHECK(!broker_uri_valid("", why));
    CHECK(!broker_uri_valid("192.168.10.5", why));            // no scheme
    CHECK(!broker_uri_valid("mqtt://", why));                 // no host
    CHECK(!broker_uri_valid("mqtt://host:", why));            // no port
    CHECK(!broker_uri_valid("mqtt://host:abc", why));         // port not a number
    CHECK(!broker_uri_valid("mqtt://host:70000", why));       // port out of range
    CHECK(!broker_uri_valid("mqtt://host:0", why));
    CHECK(!broker_uri_valid("mqtt://:1883", why));            // no host, with a port
    CHECK(!broker_uri_valid("http://host", why));             // wrong scheme
    // TLS is refused explicitly rather than attempted: no certificate bundle
    // ships, so it would fail at handshake time with nothing actionable.
    CHECK(!broker_uri_valid("mqtts://host:8883", why));
    CHECK(why.find("TLS") != std::string::npos);
}

}  // namespace

void run_tests() {
    test_base_normalisation();
    test_topic_to_command();
    test_no_second_command_grammar();
    test_payload_cannot_escape();
    test_countdown_doc();
    test_display_slice();
    test_display_slice_ignores_the_countdown_tick();
    test_broker_uri();
}
