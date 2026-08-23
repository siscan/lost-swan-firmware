// Captive-portal provisioning (spec 10.1) - the pure half.
//
// Two things live here because both are easy to get subtly wrong and neither
// needs hardware to test: the decision to enter AP mode at all, and the DNS
// hijack that makes a phone pop the sign-in sheet.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace swan {
namespace api {

// ---------------------------------------------------------------------------
// When to run a portal
// ---------------------------------------------------------------------------
struct PortalInputs {
    bool have_credentials = false;
    bool explicitly_requested = false;   // the wifi.provision command
    bool sta_connected = false;
};

// The portal runs when there are no credentials, or when somebody asked for it.
// NEVER because the network went away: a router rebooting must not put a
// display in a wall into AP mode - it would drop off the LAN, stop answering
// lost.local, and abandon a running countdown's viewers, all to solve a problem
// that fixes itself in thirty seconds.
bool portal_should_run(const PortalInputs& in);

// LOST-Swan-a8e8, from the last two bytes of the MAC: stable across reboots, so
// a phone that has joined once offers it again.
std::string portal_ssid(const uint8_t mac[6]);

// ---------------------------------------------------------------------------
// The DNS hijack
// ---------------------------------------------------------------------------
// Answer every A query with our own address, so whatever the phone asks for it
// arrives at the portal.  Anything that is not a single standard A/ANY query is
// left alone rather than answered wrongly.
//
// Returns the reply length, or 0 for "do not answer".
std::size_t dns_hijack_reply(const uint8_t* query, std::size_t len, uint32_t ip_be,
                             uint8_t* out, std::size_t out_cap);

// The captive-portal probes, by path.  Each platform wants a different answer,
// and giving the wrong one means the sheet never appears and the user is told
// the network "has no internet" instead.
enum class ProbeKind : uint8_t {
    None,        // an ordinary request; serve the portal page
    Redirect,    // iOS/macOS and Windows: a 302 to the portal is what pops it
    NoContent,   // Android wants 204 for success; anything else means captive
};
ProbeKind portal_probe_kind(std::string_view path, std::string_view host,
                            std::string_view our_host);

}  // namespace api
}  // namespace swan
