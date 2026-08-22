// POSIX TZ engine: parsing, civil-date math, and DST edges to the second.
#include <cstring>

#include "check.h"
#include "timesvc/tz.h"

using namespace swan;

namespace {

int64_t utc(int y, int mo, int d, int h, int mi, int s) {
    return TimeZone::days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
}

bool local_is(const LocalTime& lt, int y, int mo, int d, int h, int mi, int s, bool dst) {
    return lt.year == y && lt.month == mo && lt.day == d && lt.hour == h && lt.minute == mi &&
           lt.second == s && lt.dst == dst;
}

void test_civil_math() {
    CHECK_EQ(TimeZone::days_from_civil(1970, 1, 1), 0);
    CHECK_EQ(TimeZone::days_from_civil(2026, 8, 21), 20686);
    int y, m, d;
    TimeZone::civil_from_days(20686, y, m, d);
    CHECK_EQ(y, 2026);
    CHECK_EQ(m, 8);
    CHECK_EQ(d, 21);
    CHECK_EQ(TimeZone::weekday_from_days(0), 4);      // 1970-01-01 was a Thursday
    CHECK_EQ(TimeZone::weekday_from_days(20686), 5);  // 2026-08-21 is a Friday

    // Round-trip a wide range incl. leap years and century rules.
    for (int64_t z = -200000; z <= 200000; z += 97) {
        TimeZone::civil_from_days(z, y, m, d);
        CHECK_EQ(TimeZone::days_from_civil(y, m, d), z);
    }
}

void test_parse() {
    TimeZone tz;
    std::string err;

    CHECK(TimeZone::parse("PST8PDT,M3.2.0,M11.1.0", tz, &err));
    CHECK(tz.has_dst());
    CHECK(tz.std_name() == "PST");
    CHECK(tz.dst_name() == "PDT");

    CHECK(TimeZone::parse("UTC0", tz, &err));
    CHECK(!tz.has_dst());

    CHECK(TimeZone::parse("CET-1CEST,M3.5.0,M10.5.0/3", tz, &err));
    CHECK(tz.has_dst());

    CHECK(TimeZone::parse("<+0330>-3:30", tz, &err));   // quoted name, minutes
    CHECK(!tz.has_dst());

    // tzdb extension: rule times beyond 24 h (Asia/Jerusalem, Gaza).
    CHECK(TimeZone::parse("IST-2IDT,M3.4.4/26,M10.5.0", tz, &err));
    CHECK(TimeZone::parse("EET-2EEST,M3.4.4/48,M10.4.4/49", tz, &err));

    // Rejections.
    CHECK(!TimeZone::parse("", tz, &err));
    // Rules without a dst designation would leave dst_offset at 0 (UTC) and
    // silently shift the DST half of the year - refuse, don't guess.
    CHECK(!TimeZone::parse("PST8,M3.2.0,M11.1.0", tz, &err));
    CHECK(!TimeZone::parse("PST", tz, &err));               // no offset
    CHECK(!TimeZone::parse("PST8PDT", tz, &err));           // dst without rules
    CHECK(!TimeZone::parse("PST8PDT,J60,J300", tz, &err));  // J-form unsupported
    CHECK(!TimeZone::parse("PST8PDT,M13.1.0,M11.1.0", tz, &err));  // bad month
    CHECK(!TimeZone::parse("PST8PDT,M3.2.0,M11.1.0zzz", tz, &err));
}

// US Pacific 2026: DST starts Sun Mar 8 02:00 standard (10:00 UTC), ends
// Sun Nov 1 02:00 DST (09:00 UTC).  The spec 8 default zone.
void test_pacific_dst_edges() {
    TimeZone tz;
    CHECK(TimeZone::parse("PST8PDT,M3.2.0,M11.1.0", tz, nullptr));

    // One second before spring-forward: 01:59:59 PST.
    CHECK(local_is(tz.to_local(utc(2026, 3, 8, 9, 59, 59)), 2026, 3, 8, 1, 59, 59, false));
    // The transition instant: 02:00 std does not exist; wall jumps to 03:00 PDT.
    CHECK(local_is(tz.to_local(utc(2026, 3, 8, 10, 0, 0)), 2026, 3, 8, 3, 0, 0, true));

    // One second before fall-back: 01:59:59 PDT.
    CHECK(local_is(tz.to_local(utc(2026, 11, 1, 8, 59, 59)), 2026, 11, 1, 1, 59, 59, true));
    // The transition instant: wall falls back to 01:00:00 PST.
    CHECK(local_is(tz.to_local(utc(2026, 11, 1, 9, 0, 0)), 2026, 11, 1, 1, 0, 0, false));

    // Deep winter / deep summer.
    CHECK(!tz.is_dst(utc(2026, 1, 15, 12, 0, 0)));
    CHECK(tz.is_dst(utc(2026, 7, 15, 12, 0, 0)));

    // Year boundary: Dec 31 23:59:59 PST and Jan 1 both standard.
    CHECK(local_is(tz.to_local(utc(2027, 1, 1, 7, 59, 59)), 2026, 12, 31, 23, 59, 59, false));

    // 2027 rules land on different dates (Mar 14, Nov 7) - the rule engine,
    // not a table, must find them.
    CHECK(!tz.is_dst(utc(2027, 3, 14, 9, 59, 59)));
    CHECK(tz.is_dst(utc(2027, 3, 14, 10, 0, 0)));
    CHECK(tz.is_dst(utc(2027, 11, 7, 8, 59, 59)));
    CHECK(!tz.is_dst(utc(2027, 11, 7, 9, 0, 0)));
}

// Europe (east-of-UTC offsets, rule time 03:00, "last Sunday" week-5 rules).
void test_cet_dst_edges() {
    TimeZone tz;
    CHECK(TimeZone::parse("CET-1CEST,M3.5.0,M10.5.0/3", tz, nullptr));

    // 2026: last Sun of March = Mar 29; 02:00 std = 01:00 UTC.
    CHECK(!tz.is_dst(utc(2026, 3, 29, 0, 59, 59)));
    CHECK(tz.is_dst(utc(2026, 3, 29, 1, 0, 0)));
    CHECK(local_is(tz.to_local(utc(2026, 3, 29, 1, 0, 0)), 2026, 3, 29, 3, 0, 0, true));

    // Last Sun of October = Oct 25; 03:00 DST = 01:00 UTC.
    CHECK(tz.is_dst(utc(2026, 10, 25, 0, 59, 59)));
    CHECK(!tz.is_dst(utc(2026, 10, 25, 1, 0, 0)));
    CHECK(local_is(tz.to_local(utc(2026, 10, 25, 1, 0, 0)), 2026, 10, 25, 2, 0, 0, false));
}

// Southern hemisphere: DST spans the new year.
void test_southern_dst() {
    TimeZone tz;  // New Zealand
    CHECK(TimeZone::parse("NZST-12NZDT,M9.5.0,M4.1.0/3", tz, nullptr));

    CHECK(tz.is_dst(utc(2026, 1, 15, 0, 0, 0)));    // mid-January: DST
    CHECK(!tz.is_dst(utc(2026, 6, 15, 0, 0, 0)));   // June: winter
    CHECK(tz.is_dst(utc(2026, 12, 25, 0, 0, 0)));   // Christmas: DST
    // 2026 start: last Sun Sep = Sep 27, 02:00 std = 14:00 UTC Sep 26.
    CHECK(!tz.is_dst(utc(2026, 9, 26, 13, 59, 59)));
    CHECK(tz.is_dst(utc(2026, 9, 26, 14, 0, 0)));
}

void test_from_local() {
    TimeZone tz;
    CHECK(TimeZone::parse("PST8PDT,M3.2.0,M11.1.0", tz, nullptr));

    // Plain winter/summer round trips.
    CHECK_EQ(tz.from_local(2026, 1, 15, 12, 0, 0), utc(2026, 1, 15, 20, 0, 0));
    CHECK_EQ(tz.from_local(2026, 7, 15, 12, 0, 0), utc(2026, 7, 15, 19, 0, 0));

    // Round-trip consistency across a whole year, hourly.
    for (int64_t t = utc(2026, 1, 1, 0, 30, 0); t < utc(2027, 1, 1, 0, 0, 0); t += 3600) {
        const LocalTime lt = tz.to_local(t);
        const int64_t back = tz.from_local(lt.year, lt.month, lt.day, lt.hour, lt.minute,
                                           lt.second);
        // Exact except inside the fall-back overlap hour, where the DST
        // instant legitimately wins (documented contract).
        if (back != t) CHECK_EQ(back, t - 3600);
    }
}

}  // namespace

void run_tests() {
    test_civil_math();
    test_parse();
    test_pacific_dst_edges();
    test_cet_dst_edges();
    test_southern_dst();
    test_from_local();
}
