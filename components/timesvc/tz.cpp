#include "timesvc/tz.h"

namespace swan {
namespace {

constexpr int64_t DAY = 86400;

struct Cursor {
    std::string_view s;
    size_t i = 0;

    bool done() const { return i >= s.size(); }
    char peek() const { return done() ? '\0' : s[i]; }

    bool name(std::string& out) {
        out.clear();
        if (peek() == '<') {  // quoted form <+03>
            ++i;
            while (!done() && s[i] != '>') out += s[i++];
            if (done()) return false;
            ++i;
        } else {
            while (!done() && ((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z'))) {
                out += s[i++];
            }
        }
        return out.size() >= 3;
    }

    bool number(int& out, int max_digits = 3) {
        int v = 0, digits = 0;
        while (!done() && s[i] >= '0' && s[i] <= '9' && digits < max_digits) {
            v = v * 10 + (s[i] - '0');
            ++i;
            ++digits;
        }
        if (digits == 0) return false;
        out = v;
        return true;
    }

    // [+|-]hh[:mm[:ss]] -> seconds; POSIX sign kept (positive = west).
    bool offset(int32_t& out) {
        int sign = 1;
        if (peek() == '-') {
            sign = -1;
            ++i;
        } else if (peek() == '+') {
            ++i;
        }
        int h = 0, m = 0, sec = 0;
        if (!number(h)) return false;
        if (peek() == ':') {
            ++i;
            if (!number(m, 2)) return false;
            if (peek() == ':') {
                ++i;
                if (!number(sec, 2)) return false;
            }
        }
        if (h > 24 || m > 59 || sec > 59) return false;
        out = sign * (h * 3600 + m * 60 + sec);
        return true;
    }
};

}  // namespace

int64_t TimeZone::days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = static_cast<unsigned>((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void TimeZone::civil_from_days(int64_t z, int& y, int& m, int& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t yy = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<int>(mp + (mp < 10 ? 3 : -9));
    y = static_cast<int>(yy + (m <= 2));
}

int TimeZone::weekday_from_days(int64_t z) {
    return static_cast<int>(z >= -4 ? (z + 4) % 7 : (z + 5) % 7 + 6);
}

bool TimeZone::parse(std::string_view tz, TimeZone& out, std::string* err) {
    out = TimeZone{};
    Cursor c{tz, 0};
    auto fail = [&](const char* why) {
        if (err != nullptr) *err = why;
        return false;
    };

    if (!c.name(out.std_name_)) return fail("bad std name");
    if (!c.offset(out.std_offset_)) return fail("bad std offset");

    if (c.done()) return true;  // no DST

    if (c.peek() != ',') {  // dst name follows
        if (!c.name(out.dst_name_)) return fail("bad dst name");
        // Optional explicit dst offset; default one hour ahead of std.
        if (!c.done() && c.peek() != ',') {
            if (!c.offset(out.dst_offset_)) return fail("bad dst offset");
        } else {
            out.dst_offset_ = out.std_offset_ - 3600;
        }
    }

    if (c.done()) {
        // DST name but no rules: refuse rather than guess (header comment).
        return fail("dst named but no transition rules");
    }
    if (c.peek() != ',') return fail("expected ','");
    ++c.i;

    auto rule = [&](Rule& r) {
        if (c.peek() != 'M') return fail("only M-rule dates supported");
        ++c.i;
        if (!c.number(r.month, 2) || r.month < 1 || r.month > 12) return fail("bad rule month");
        if (c.peek() != '.') return fail("bad rule");
        ++c.i;
        if (!c.number(r.week, 1) || r.week < 1 || r.week > 5) return fail("bad rule week");
        if (c.peek() != '.') return fail("bad rule");
        ++c.i;
        if (!c.number(r.day, 1) || r.day > 6) return fail("bad rule day");
        if (c.peek() == '/') {
            ++c.i;
            int32_t t = 0;
            if (!c.offset(t)) return fail("bad rule time");
            r.at = t;
        }
        return true;
    };

    if (!rule(out.start_)) return false;
    if (c.peek() != ',') return fail("expected second rule");
    ++c.i;
    if (!rule(out.end_)) return false;
    if (!c.done()) return fail("trailing garbage");

    out.has_dst_ = true;
    return true;
}

int64_t TimeZone::rule_epoch_local(const Rule& r, int year) const {
    // Day-of-month of the r.week-th r.day weekday (5 = last).
    const int64_t first = days_from_civil(year, r.month, 1);
    const int first_wd = weekday_from_days(first);
    int dom = 1 + ((r.day - first_wd) % 7 + 7) % 7 + (r.week - 1) * 7;

    // Clamp week 5 ("last") into the month.
    const int days_in_month =
        static_cast<int>(days_from_civil(year + (r.month == 12), r.month == 12 ? 1 : r.month + 1, 1) -
                         first);
    while (dom > days_in_month) dom -= 7;

    return (first + dom - 1) * DAY + r.at;
}

void TimeZone::dst_span_utc(int year, int64_t& start_utc, int64_t& end_utc) const {
    // POSIX: the start rule's time is wall STANDARD time, the end rule's is
    // wall DST time.  utc = local + offset (offset positive west).
    start_utc = rule_epoch_local(start_, year) + std_offset_;
    end_utc = rule_epoch_local(end_, year) + dst_offset_;
}

bool TimeZone::is_dst(int64_t utc) const {
    if (!has_dst_) return false;

    // Approximate the local year; correct at year boundaries by checking the
    // neighbouring year's span too.
    int y, m, d;
    civil_from_days((utc - std_offset_) / DAY - ((utc - std_offset_) % DAY < 0 ? 1 : 0), y, m, d);

    for (int yy = y - 1; yy <= y + 1; ++yy) {
        int64_t s, e;
        dst_span_utc(yy, s, e);
        if (s < e) {  // northern hemisphere: DST within one calendar year
            if (utc >= s && utc < e) return true;
        } else {
            // Southern hemisphere: DST runs from this year's start rule to
            // NEXT year's end rule.  The three-year sweep covers both halves.
            int64_t s_next, e_next;
            dst_span_utc(yy + 1, s_next, e_next);
            (void)s_next;
            if (utc >= s && utc < e_next) return true;
        }
    }
    return false;
}

LocalTime TimeZone::to_local(int64_t utc) const {
    const bool dst = is_dst(utc);
    const int64_t local = utc - (dst ? dst_offset_ : std_offset_);

    int64_t days = local / DAY;
    int64_t sod = local % DAY;
    if (sod < 0) {
        sod += DAY;
        --days;
    }

    LocalTime out;
    civil_from_days(days, out.year, out.month, out.day);
    out.weekday = weekday_from_days(days);
    out.hour = static_cast<int>(sod / 3600);
    out.minute = static_cast<int>((sod / 60) % 60);
    out.second = static_cast<int>(sod % 60);
    out.dst = dst;
    return out;
}

int64_t TimeZone::from_local(int year, int month, int day, int hour, int minute,
                             int second) const {
    const int64_t wall =
        days_from_civil(year, month, day) * DAY + hour * 3600 + minute * 60 + second;
    // Try DST first (fall-back overlap: DST instant wins), then standard.
    if (has_dst_) {
        const int64_t as_dst = wall + dst_offset_;
        if (is_dst(as_dst)) return as_dst;
    }
    return wall + std_offset_;
}

}  // namespace swan
