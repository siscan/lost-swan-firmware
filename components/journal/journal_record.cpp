// The pure half of the journal: the record format and the rotation policy.
#include "journal/journal.h"

#include <cstdio>
#include <cstring>

#include "ring/json_lite.h"
#include "ring/json_write.h"

namespace swan {
namespace journal {

namespace {
struct KindName {
    Event::Kind k;
    const char* name;
};
// Short, because every one of these is written to flash and read back by a
// browser that renders them as printout lines.
constexpr KindName kNames[] = {
    {Event::Kind::Boot, "boot"},
    {Event::Kind::CountdownExecute, "execute"},
    {Event::Kind::CountdownStart, "start"},
    {Event::Kind::CountdownReset, "reset"},
    {Event::Kind::CountdownCancel, "cancel"},
    {Event::Kind::CountdownZero, "zero"},
    {Event::Kind::Fault, "fault"},
    {Event::Kind::Recover, "recover"},
    {Event::Kind::ModeChange, "mode"},
    {Event::Kind::Maintenance, "maint"},
    {Event::Kind::ColumnMode, "column"},
    {Event::Kind::Reveal, "reveal"},
};
}  // namespace

const char* kind_name(Event::Kind k) {
    for (const KindName& n : kNames) {
        if (n.k == k) return n.name;
    }
    return "?";
}

bool kind_from_name(const char* s, Event::Kind& out) {
    if (s == nullptr) return false;
    for (const KindName& n : kNames) {
        if (std::strcmp(s, n.name) == 0) {
            out = n.k;
            return true;
        }
    }
    return false;
}

std::string encode(const Event& e) {
    json::Writer w;
    w.obj()
        .kv("t", e.utc_s)
        .kv("u", static_cast<int64_t>(e.uptime_s))
        .kv("e", kind_name(e.kind));
    if (e.column >= 0) w.kv("col", static_cast<int64_t>(e.column));
    if (e.seq != 0) w.kv("seq", static_cast<int64_t>(e.seq));
    if (e.who[0] != '\0') w.kv("by", e.who);
    if (e.detail[0] != '\0') w.kv("d", e.detail);
    w.end_obj();
    std::string out = w.take();
    out.push_back('\n');
    return out;
}

bool decode(const std::string& line, Event& out) {
    if (line.empty()) return false;
    json::Value v;
    // A power cut mid-append leaves a partial last line.  That is expected, not
    // corruption: drop the line, keep the file.
    if (!json::parse(line, v, nullptr)) return false;
    const json::Value* kind = v.get("e");
    if (kind == nullptr) return false;
    if (!kind_from_name(std::string(kind->as_str()).c_str(), out.kind)) return false;
    if (const json::Value* t = v.get("t")) out.utc_s = t->as_int(0);
    if (const json::Value* u = v.get("u")) out.uptime_s = static_cast<uint32_t>(u->as_int(0));
    if (const json::Value* c = v.get("col")) out.column = static_cast<int8_t>(c->as_int(-1));
    if (const json::Value* s = v.get("seq")) out.seq = static_cast<uint32_t>(s->as_int(0));
    if (const json::Value* b = v.get("by")) {
        std::snprintf(out.who, sizeof out.who, "%s", std::string(b->as_str()).c_str());
    }
    if (const json::Value* d = v.get("d")) {
        std::snprintf(out.detail, sizeof out.detail, "%s", std::string(d->as_str()).c_str());
    }
    return true;
}

bool needs_rotation(const RotationPolicy& p, std::size_t entries, std::size_t bytes) {
    return entries >= p.max_entries || bytes >= p.max_bytes;
}

std::string compact(const RotationPolicy& p, const std::vector<std::string>& lines) {
    const std::size_t keep = p.keep_entries < lines.size() ? p.keep_entries : lines.size();
    const std::size_t from = lines.size() - keep;
    std::string out;
    std::size_t need = 0;
    for (std::size_t i = from; i < lines.size(); ++i) need += lines[i].size() + 1;
    out.reserve(need);
    for (std::size_t i = from; i < lines.size(); ++i) {
        out += lines[i];
        if (out.empty() || out.back() != '\n') out.push_back('\n');
    }
    return out;
}

}  // namespace journal
}  // namespace swan
