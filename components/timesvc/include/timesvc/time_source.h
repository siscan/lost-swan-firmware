// The clock's view of time - an interface so everything downstream (clock
// render, countdown deadline math, cues) is host-testable with a fake, and
// nothing outside timesvc touches SNTP or the RTC (spec 8, Phase 2 rule:
// anything touching WiFi/NTP goes behind an interface).
#pragma once

#include <cstdint>

namespace swan {

class TimeSource {
public:
    virtual ~TimeSource() = default;

    // UTC seconds since the epoch.  Free-runs when the network is gone.
    virtual int64_t now_utc() = 0;

    // True once SNTP has synced at least once since boot.  Never goes back to
    // false on WiFi loss - the clock free-runs (spec 7.1/8).
    virtual bool valid() = 0;
};

}  // namespace swan
