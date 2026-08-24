#include "webapi/mqtt_bridge.h"

#include "ring/json_lite.h"
#include "ring/json_write.h"

namespace swan {
namespace api {
namespace {

bool is_command_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '.' || c == '_';
}

}  // namespace

std::string normalize_base(std::string_view configured) {
    size_t b = 0;
    size_t e = configured.size();
    while (b < e && configured[b] == '/') ++b;
    while (e > b && configured[e - 1] == '/') --e;
    if (b >= e) return "swan/";
    return std::string(configured.substr(b, e - b)) + "/";
}

CmdParse command_from_topic(std::string_view base, std::string_view topic,
                            std::string_view payload, std::string& out_body,
                            std::string& out_command) {
    out_body.clear();
    out_command.clear();

    if (topic.size() <= base.size() || topic.compare(0, base.size(), base) != 0) {
        return CmdParse::NotOurs;
    }
    std::string_view rest = topic.substr(base.size());
    constexpr std::string_view kCmd = "cmd/";
    if (rest.size() < kCmd.size() || rest.compare(0, kCmd.size(), kCmd) != 0) {
        return CmdParse::NotOurs;
    }
    const std::string_view cmd = rest.substr(kCmd.size());
    if (cmd.empty()) return CmdParse::EmptyCommand;

    // No arguments in topic segments, ever: `swan/cmd/motion.column/2` is a
    // second command grammar, and this is where it gets refused rather than
    // half-understood.  A plausible command name and nothing else.
    if (cmd.size() > 40) return CmdParse::BadCommand;
    for (const char c : cmd) {
        if (!is_command_char(c)) return CmdParse::BadCommand;
    }
    out_command.assign(cmd);

    json::Writer w;
    w.obj().kv("cmd", cmd);
    if (!payload.empty()) {
        // Pass structured payloads through untouched; quote anything else.  A
        // bare `clock` on mode.set is the shape the spec promises, and it is
        // also what a human typing mosquitto_pub will send.
        json::Value probe;
        const bool structured = json::parse(payload, probe, nullptr);
        if (structured) {
            w.kv_raw("payload", payload);
        } else {
            w.kv("payload", payload);
        }
    }
    w.end_obj();
    out_body = w.take();
    return CmdParse::Ok;
}

std::string countdown_doc(const char* phase, int64_t target_utc, const char* set_by,
                          uint32_t seq) {
    json::Writer w;
    w.obj()
        .kv("state", phase)
        .kv("target", target_utc)
        .kv("set_by", set_by)
        .kv("seq", static_cast<int64_t>(seq))
        .end_obj();
    return w.take();
}

std::string display_slice(std::string_view state_doc) {
    const size_t k = state_doc.find("\"mode\"");
    std::string out;
    if (k == std::string_view::npos) {
        out.assign(state_doc);
    } else {
        const size_t e = state_doc.find(",\"sys\":", k);
        out.assign(state_doc.substr(k, e == std::string_view::npos ? std::string_view::npos
                                                                   : e - k));
    }
    // Elide the ticking value rather than the whole field, so that a document
    // which stops carrying it still compares differently from one that does.
    constexpr std::string_view kKey = "\"remaining_s\":";
    const size_t r = out.find(kKey);
    if (r != std::string::npos) {
        size_t v = r + kKey.size();
        while (v < out.size() && (out[v] == '-' || (out[v] >= '0' && out[v] <= '9'))) ++v;
        out.replace(r + kKey.size(), v - (r + kKey.size()), "*");
    }
    return out;
}

bool broker_uri_valid(std::string_view uri, std::string& why) {
    why.clear();
    if (uri.empty()) {
        why = "empty";
        return false;
    }
    const size_t sep = uri.find("://");
    if (sep == std::string_view::npos) {
        // esp-mqtt would take a bare host and fail somewhere unhelpful.
        why = "needs a scheme, e.g. mqtt://host:1883";
        return false;
    }
    const std::string_view scheme = uri.substr(0, sep);
    if (scheme == "mqtts" || scheme == "wss") {
        // No certificate bundle ships, so TLS would fail at handshake time
        // with an error nobody can act on from the Settings page.
        why = "TLS is not supported in this build";
        return false;
    }
    if (scheme != "mqtt" && scheme != "ws") {
        why = "scheme must be mqtt:// or ws://";
        return false;
    }
    std::string_view host = uri.substr(sep + 3);
    const size_t slash = host.find('/');
    if (slash != std::string_view::npos) host = host.substr(0, slash);
    if (host.empty()) {
        why = "no host";
        return false;
    }
    const size_t colon = host.rfind(':');
    if (colon != std::string_view::npos) {
        const std::string_view port = host.substr(colon + 1);
        if (port.empty()) {
            why = "no port after ':'";
            return false;
        }
        long value = 0;
        for (const char c : port) {
            if (c < '0' || c > '9') {
                why = "port is not a number";
                return false;
            }
            value = value * 10 + (c - '0');
            if (value > 65535) {
                why = "port out of range";
                return false;
            }
        }
        if (value == 0) {
            why = "port out of range";
            return false;
        }
        host = host.substr(0, colon);
        if (host.empty()) {
            why = "no host";
            return false;
        }
    }
    return true;
}

// Read-only: the prop states its own liveness and we believe the flag, but
// nothing else.  A payload from another machine reaches a browser verbatim, so
// `fw` is length-bounded here rather than at the far end, and the node budget
// is the untrusted one.
bool parse_prop_presence(std::string_view payload, PropPresence& out) {
    json::Value v;
    if (!json::parse(payload, v, nullptr)) return false;
    if (v.type != json::Type::Object) return false;
    const json::Value* on = v.get("online");
    if (on == nullptr || on->type != json::Type::Bool) return false;

    PropPresence p;
    p.seen = true;
    p.online = on->boolean;
    if (const json::Value* fw = v.get("fw")) {
        // A non-string fw is ignored rather than rejected: the liveness flag is
        // the part that matters and a version string is decoration.
        if (fw->type == json::Type::Str) {
            const std::string_view sv = fw->as_str();
            const size_t n = sv.size() < PROP_FW_MAX ? sv.size() : PROP_FW_MAX;
            for (size_t i = 0; i < n; ++i) {
                // Printable ASCII only - this goes into a JSON document and
                // then into a browser.
                const char c = sv[i];
                p.fw[i] = (c >= 0x20 && c < 0x7F) ? c : '?';
            }
            p.fw[n] = '\0';
        }
    }
    out = p;
    return true;
}

}  // namespace api
}  // namespace swan
