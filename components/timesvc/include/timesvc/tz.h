// POSIX TZ string engine - pure, no IDF, no libc timezone dependence.
//
// Owned code instead of localtime_r because the clock's DST behaviour must be
// host-testable to the second and identical on target: newlib, glibc and
// MinGW's UCRT disagree on M-rule TZ strings (UCRT does not parse them at
// all).  Scope: the POSIX 'stdoffset[dst[offset][,start[/time],end[/time]]]'
// form with M-rule dates (Mm.w.d) - which is what every real TZ string uses,
// including the default PST8PDT,M3.2.0,M11.1.0 (spec 8).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace swan {

struct LocalTime {
    int year;     // e.g. 2026
    int month;    // 1..12
    int day;      // 1..31
    int hour;     // 0..23
    int minute;   // 0..59
    int second;   // 0..59
    int weekday;  // 0 = Sunday
    bool dst;
};

class TimeZone {
public:
    // Parses a POSIX TZ string.  A dst name without explicit M-rules is
    // rejected (POSIX leaves the rules implementation-defined; guessing US
    // rules silently is exactly the kind of thing that bites in November).
    static bool parse(std::string_view tz, TimeZone& out, std::string* err = nullptr);

    LocalTime to_local(int64_t utc_epoch) const;
    bool is_dst(int64_t utc_epoch) const;

    // Local wall-clock -> UTC epoch, honouring DST at that local time.  In the
    // spring-forward gap the (nonexistent) time maps as standard time; in the
    // fall-back overlap the DST (earlier) instant wins.  Fine for cue/schedule
    // math; the display only ever converts UTC -> local.
    int64_t from_local(int year, int month, int day, int hour, int minute, int second) const;

    bool has_dst() const { return has_dst_; }
    const std::string& std_name() const { return std_name_; }
    const std::string& dst_name() const { return dst_name_; }

    // Civil-date helpers (Howard Hinnant's algorithms), exposed for tests and
    // the countdown schedule math.
    static int64_t days_from_civil(int y, int m, int d);
    static void civil_from_days(int64_t z, int& y, int& m, int& d);
    static int weekday_from_days(int64_t z);  // 0 = Sunday

private:
    struct Rule {
        int month = 0;        // 1..12
        int week = 0;         // 1..5, 5 = last
        int day = 0;          // 0..6, 0 = Sunday
        int32_t at = 2 * 3600;  // transition time-of-day, seconds
    };

    std::string std_name_, dst_name_;
    int32_t std_offset_ = 0;  // seconds WEST of UTC (POSIX sign): utc = local + offset
    int32_t dst_offset_ = 0;
    bool has_dst_ = false;
    Rule start_, end_;

    int64_t rule_epoch_local(const Rule& r, int year) const;  // local seconds
    void dst_span_utc(int year, int64_t& start_utc, int64_t& end_utc) const;
};

}  // namespace swan
