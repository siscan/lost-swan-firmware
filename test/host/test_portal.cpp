// Captive-portal provisioning (spec 10.1), the pure half.
//
// The DNS responder parses a packet from anyone within radio range of an open
// access point, so it is tested the way the ring upload is: with the malformed
// cases first.
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "webapi/portal.h"

using namespace swan;
using namespace swan::api;

namespace {

// A well-formed query for `name`, e.g. "captive.apple.com".
std::vector<uint8_t> query(const std::string& name, uint16_t qtype = 1, uint16_t qclass = 1) {
    std::vector<uint8_t> q = {0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0};
    std::size_t start = 0;
    while (start <= name.size()) {
        const std::size_t dot = name.find('.', start);
        const std::string label =
            name.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        q.push_back(static_cast<uint8_t>(label.size()));
        q.insert(q.end(), label.begin(), label.end());
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    q.push_back(0);
    q.push_back(static_cast<uint8_t>(qtype >> 8));
    q.push_back(static_cast<uint8_t>(qtype & 0xFF));
    q.push_back(static_cast<uint8_t>(qclass >> 8));
    q.push_back(static_cast<uint8_t>(qclass & 0xFF));
    return q;
}

void test_when_the_portal_runs() {
    PortalInputs in;
    // Nothing configured: this is the only automatic case.
    CHECK(portal_should_run(in));

    // Credentials exist. THE case that matters: a router reboot must not put a
    // display in a wall into AP mode - it would drop off the LAN, stop
    // answering lost.local, and abandon a running countdown's viewers, to fix
    // something that fixes itself.
    in.have_credentials = true;
    in.sta_connected = false;
    CHECK(!portal_should_run(in));
    in.sta_connected = true;
    CHECK(!portal_should_run(in));

    // Asked for, explicitly, which is the recovery path.
    in.explicitly_requested = true;
    CHECK(portal_should_run(in));
}

void test_ssid_is_stable() {
    const uint8_t mac[6] = {0x10, 0xbd, 0xa3, 0xdd, 0xa8, 0xe8};
    CHECK(portal_ssid(mac) == "LOST-Swan-a8e8");
    // Stable across reboots, so a phone that joined once offers it again.
    CHECK(portal_ssid(mac) == portal_ssid(mac));
    const uint8_t other[6] = {0, 0, 0, 0, 0x01, 0x02};
    CHECK(portal_ssid(other) == "LOST-Swan-0102");
}

void test_dns_answers_an_a_query() {
    const std::vector<uint8_t> q = query("captive.apple.com");
    uint8_t out[512];
    const uint32_t ip = 0x0104A8C0;   // 192.168.4.1, network order
    const std::size_t n = dns_hijack_reply(q.data(), q.size(), ip, out, sizeof out);
    CHECK(n > q.size());

    CHECK(out[0] == 0x12 && out[1] == 0x34);            // the id is echoed
    CHECK(out[2] == 0x81 && out[3] == 0x80);            // response
    CHECK(out[4] == 0 && out[5] == 1);                  // one question
    CHECK(out[6] == 0 && out[7] == 1);                  // one answer
    CHECK(std::memcmp(out + 12, q.data() + 12, q.size() - 12) == 0);

    const uint8_t* a = out + q.size();
    CHECK(a[0] == 0xC0 && a[1] == 0x0C);                // name pointer
    CHECK(a[2] == 0 && a[3] == 1);                      // A
    CHECK(a[4] == 0 && a[5] == 1);                      // IN
    // TTL 0: the moment the display joins a real network, a cached answer must
    // not send the phone back to a portal that no longer exists.
    CHECK(a[6] == 0 && a[7] == 0 && a[8] == 0 && a[9] == 0);
    CHECK(a[10] == 0 && a[11] == 4);
    CHECK(std::memcmp(a + 12, &ip, 4) == 0);
}

void test_dns_refuses_what_it_should_not_answer() {
    uint8_t out[512];
    const uint32_t ip = 0x0104A8C0;

    // A truncated packet, at every length. This is a UDP socket on an open AP:
    // anyone in radio range can send whatever they like.
    const std::vector<uint8_t> q = query("captive.apple.com");
    for (std::size_t n = 0; n < q.size(); ++n) {
        CHECK_EQ(dns_hijack_reply(q.data(), n, ip, out, sizeof out), 0u);
    }

    // A response, not a query - answering it would be a packet-amplification
    // reflector.
    std::vector<uint8_t> resp = q;
    resp[2] |= 0x80;
    CHECK_EQ(dns_hijack_reply(resp.data(), resp.size(), ip, out, sizeof out), 0u);

    // Not a standard query (an UPDATE, say).
    std::vector<uint8_t> upd = q;
    upd[2] = static_cast<uint8_t>(upd[2] | (5 << 3));
    CHECK_EQ(dns_hijack_reply(upd.data(), upd.size(), ip, out, sizeof out), 0u);

    // More than one question: the reply shape below assumes exactly one.
    std::vector<uint8_t> two = q;
    two[5] = 2;
    CHECK_EQ(dns_hijack_reply(two.data(), two.size(), ip, out, sizeof out), 0u);

    // A compression pointer in a QUESTION. There is no prior name to point at,
    // so this is malformed or hostile - following it reads outside the packet.
    std::vector<uint8_t> ptr = {0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0,
                                0xC0, 0x0C, 0, 1, 0, 1};
    CHECK_EQ(dns_hijack_reply(ptr.data(), ptr.size(), ip, out, sizeof out), 0u);

    // A label that runs past the end of the packet.
    std::vector<uint8_t> over = {0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0, 0x40};
    CHECK_EQ(dns_hijack_reply(over.data(), over.size(), ip, out, sizeof out), 0u);

    // AAAA: answering it with an A record is a malformed reply. Saying nothing
    // lets the resolver fall back to IPv4, which is what we want.
    const std::vector<uint8_t> aaaa = query("captive.apple.com", 28);
    CHECK_EQ(dns_hijack_reply(aaaa.data(), aaaa.size(), ip, out, sizeof out), 0u);
    // A non-IN class.
    const std::vector<uint8_t> chaos = query("version.bind", 1, 3);
    CHECK_EQ(dns_hijack_reply(chaos.data(), chaos.size(), ip, out, sizeof out), 0u);

    // No room to write the answer: refuse rather than overrun.
    CHECK_EQ(dns_hijack_reply(q.data(), q.size(), ip, out, q.size() + 4), 0u);
    // ANY is answered, since some resolvers still send it.
    const std::vector<uint8_t> any = query("captive.apple.com", 255);
    CHECK(dns_hijack_reply(any.data(), any.size(), ip, out, sizeof out) > 0);
}

void test_probe_kinds() {
    // Android wants a 204 for "online"; anything else is what tells it a
    // portal is in the way.
    CHECK(portal_probe_kind("/generate_204", "connectivitycheck.gstatic.com", "lost.local") ==
          ProbeKind::NoContent);
    CHECK(portal_probe_kind("/gen_204", "www.gstatic.com", "lost.local") ==
          ProbeKind::NoContent);
    // iOS / macOS and Windows: a redirect is the documented way to raise the
    // sign-in sheet.
    for (const char* p : {"/hotspot-detect.html", "/success.txt", "/connecttest.txt",
                          "/ncsi.txt", "/redirect", "/canonical.html"}) {
        CHECK(portal_probe_kind(p, "captive.apple.com", "lost.local") == ProbeKind::Redirect);
    }
    // Somebody typing our own address gets the page, not a redirect loop.
    CHECK(portal_probe_kind("/hotspot-detect.html", "lost.local", "lost.local") ==
          ProbeKind::None);
    CHECK(portal_probe_kind("/", "192.168.4.1", "lost.local") == ProbeKind::None);
    CHECK(portal_probe_kind("/app.js", "captive.apple.com", "lost.local") == ProbeKind::None);
}

}  // namespace

void run_tests() {
    test_when_the_portal_runs();
    test_ssid_is_stable();
    test_dns_answers_an_a_query();
    test_dns_refuses_what_it_should_not_answer();
    test_probe_kinds();
}
