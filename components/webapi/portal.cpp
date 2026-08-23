#include "webapi/portal.h"

#include <cstdio>
#include <cstring>

namespace swan {
namespace api {
namespace {

constexpr uint16_t QTYPE_A = 1;
constexpr uint16_t QTYPE_ANY = 255;
constexpr uint16_t QCLASS_IN = 1;

uint16_t rd16be(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}
void wr16be(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v & 0xFF);
}

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

bool portal_should_run(const PortalInputs& in) {
    if (in.explicitly_requested) return true;
    if (in.have_credentials) return false;
    // No credentials and no request.  Note this stays true while the STA is
    // trying: "no credentials" cannot become "connected".
    return !in.sta_connected;
}

std::string portal_ssid(const uint8_t mac[6]) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "LOST-Swan-%02x%02x", mac[4], mac[5]);
    return buf;
}

std::size_t dns_hijack_reply(const uint8_t* query, std::size_t len, uint32_t ip_be,
                             uint8_t* out, std::size_t out_cap) {
    // Header is 12 bytes; anything shorter is not a query.
    if (query == nullptr || out == nullptr || len < 12 + 5) return 0;
    const uint16_t flags = rd16be(query + 2);
    if ((flags & 0x8000) != 0) return 0;          // already a response
    if (((flags >> 11) & 0x0F) != 0) return 0;    // not a standard query
    if (rd16be(query + 4) != 1) return 0;         // exactly one question

    // Walk the QNAME to find the question's end.  Refuse compression pointers:
    // a query has no prior name to point at, so one here is malformed or
    // hostile, and following it would read outside the packet.
    std::size_t i = 12;
    while (i < len) {
        const uint8_t l = query[i];
        if (l == 0) {
            ++i;
            break;
        }
        if ((l & 0xC0) != 0) return 0;
        i += static_cast<std::size_t>(l) + 1;
        if (i > len) return 0;
    }
    if (i + 4 > len) return 0;
    const uint16_t qtype = rd16be(query + i);
    const uint16_t qclass = rd16be(query + i + 2);
    i += 4;
    // Only A/ANY in IN.  A AAAA query answered with an A record is a malformed
    // reply; leaving it unanswered lets the resolver fall back to IPv4, which
    // is what we want.
    if ((qtype != QTYPE_A && qtype != QTYPE_ANY) || qclass != QCLASS_IN) return 0;

    const std::size_t answer_len = 2 + 2 + 2 + 4 + 2 + 4;   // ptr,type,class,ttl,rdlen,rdata
    if (out_cap < i + answer_len) return 0;

    std::memcpy(out, query, i);
    wr16be(out + 2, 0x8180);   // response, recursion available
    wr16be(out + 6, 1);        // one answer
    wr16be(out + 8, 0);
    wr16be(out + 10, 0);

    uint8_t* a = out + i;
    wr16be(a, 0xC00C);         // a pointer to the question's name
    wr16be(a + 2, QTYPE_A);
    wr16be(a + 4, QCLASS_IN);
    // TTL 0: the moment the display joins a real network this answer must not
    // linger in the phone's cache and send it back to a portal that is gone.
    a[6] = a[7] = a[8] = a[9] = 0;
    wr16be(a + 10, 4);
    std::memcpy(a + 12, &ip_be, 4);
    return i + answer_len;
}

ProbeKind portal_probe_kind(std::string_view path, std::string_view host,
                            std::string_view our_host) {
    // Somebody typing our own address wants the portal, not a redirect loop.
    if (!our_host.empty() && host == our_host) return ProbeKind::None;

    // Android and Chrome OS: 204 means "you are online". Answering anything
    // else is what tells them a portal is in the way.
    if (path == "/generate_204" || path == "/gen_204" ||
        ends_with(path, "/generate_204")) {
        return ProbeKind::NoContent;
    }
    // iOS / macOS expect the literal success page; Windows expects
    // "Microsoft Connect Test". Redirecting either is the documented way to
    // raise the sign-in sheet.
    if (path == "/hotspot-detect.html" || path == "/library/test/success.html" ||
        path == "/success.txt" || path == "/connecttest.txt" || path == "/ncsi.txt" ||
        path == "/redirect" || path == "/canonical.html" || path == "/fwlink" ||
        path == "/fwlink/") {
        return ProbeKind::Redirect;
    }
    return ProbeKind::None;
}

}  // namespace api
}  // namespace swan
