// The MQTT transport's pure half (spec 10.2a, 10.3).
//
// MQTT adds a TRANSPORT, not a command set: `swan/cmd/<command>` plus a JSON
// payload becomes the same {"cmd":…,"payload":…} document the web UI posts, and
// goes to the same api::handle_command.  There are no MQTT-only commands and no
// arguments in topic segments - a second topic grammar would be a second
// command set, and then there would be two places to get validation wrong.
//
// Everything here is pure so the host tests cover the parsing that a broker
// would otherwise be needed to exercise.  The IDF client lives in
// components/net/mqtt.cpp and contains no policy.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace swan {
namespace api {

// ---------------------------------------------------------------------------
// Topics
// ---------------------------------------------------------------------------
// The configured base, normalised to exactly one trailing slash so every topic
// is base + leaf and nothing has to remember which side owns the separator.
// An empty or all-slash base yields the spec default, "swan/".
std::string normalize_base(std::string_view configured);

// Leaf topics under the base (spec 10.3).
inline constexpr const char* TOPIC_STATE = "state";
inline constexpr const char* TOPIC_COUNTDOWN = "countdown";
inline constexpr const char* TOPIC_EVENT = "event";
inline constexpr const char* TOPIC_AVAILABILITY = "availability";
inline constexpr const char* TOPIC_CMD_WILDCARD = "cmd/#";
inline constexpr const char* PAYLOAD_ONLINE = "online";
inline constexpr const char* PAYLOAD_OFFLINE = "offline";

// ---------------------------------------------------------------------------
// Inbound
// ---------------------------------------------------------------------------
enum class CmdParse : uint8_t {
    Ok,
    NotOurs,       // the topic is not <base>cmd/<something>
    EmptyCommand,  // "<base>cmd/" with nothing after it
    BadCommand,    // the command name is not a plausible identifier
};

// Turn `<base>cmd/<command>` plus a payload into the dispatcher's document.
//
// The topic and payload arrive as pointers into esp-mqtt's receive buffer and
// are NOT null-terminated, which is why both are views and why nothing here
// calls a C string function on them.
//
// A bare payload is accepted where the command takes a single value, exactly
// as the web UI's POST body already allows: `swan/cmd/mode.set` with `clock`
// becomes {"cmd":"mode.set","payload":"clock"}.  A payload that already parses
// as JSON is passed through unquoted; anything else is quoted as a string.  An
// empty payload yields a command with no payload at all, which is what
// `countdown.cancel` and `system.reboot` want.
CmdParse command_from_topic(std::string_view base, std::string_view topic,
                            std::string_view payload, std::string& out_body,
                            std::string& out_command);

// ---------------------------------------------------------------------------
// Outbound
// ---------------------------------------------------------------------------
// The retained deadline document (spec 7.3): {state, target, set_by, seq}.
// Separate from the state payload because a terminal prop subscribes to this
// alone - it renders from the absolute deadline and needs nothing else, so a
// 1.5 KB state document at 1 Hz would be pure cost to it.
std::string countdown_doc(const char* phase, int64_t target_utc, const char* set_by,
                          uint32_t seq);

// The part of the state document that decides "has anything changed".
//
// Two classes of field are excluded, for the same reason and with different
// histories:
//
//   sys.*            free heap and uptime jitter on every tick.  Including
//                    them once pinned the /ws push at its 5 Hz cap for ever -
//                    measured as 28 pushes in 6.5 s with the display sitting
//                    still.
//   cd.remaining_s   derived from cd.target, and it counts down.  Including it
//                    rewrote the RETAINED state topic once a second for the
//                    whole 108-minute run, which makes Home Assistant
//                    re-evaluate every template that reads it, on a device
//                    whose radio has better uses for the airtime.  The target
//                    is absolute; a peer computes the remainder itself, which
//                    spec 7.3 requires of it anyway.
//
// Returns a copy rather than a view: the remaining_s value has to be elided
// from the middle, and a view cannot express that.
std::string display_slice(std::string_view state_doc);

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
// Is this a broker URI the client can actually use?  Rejects the forms that
// otherwise fail deep inside esp-mqtt with an unhelpful error: a bare host with
// no scheme, an empty host, and (for now) any TLS scheme, since no certificate
// bundle ships.
bool broker_uri_valid(std::string_view uri, std::string& why);

}  // namespace api
}  // namespace swan
