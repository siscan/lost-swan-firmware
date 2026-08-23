// Per-column mode and maintenance state (spec 5.9).  Pure - host-tested.
//
// This is not a development scaffold.  It is how the display behaves for its
// whole life, including a repair years from now: one column real and four
// simulated during build-out, one disabled and four real during a repair.
//
// The distinction that matters: DISABLED is something a person sets, never
// something the firmware infers.  A fault stays a fault and stays visible
// until it is cleared by hand.  Failing safe is the default; excusing a broken
// column is a deliberate act.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "hal/pins.h"  // N_COLUMNS - pure, cstdint only

namespace swan {

enum class ColumnMode : uint8_t {
    Real = 0,      // drives real STEP pins and reads a real Hall
    Sim = 1,       // modelled drum; STEP is not driven and the Hall is synthetic
    Disabled = 2,  // parked, excluded from frames, never homed, never retried
};

const char* column_mode_name(ColumnMode m);
bool column_mode_from_name(std::string_view s, ColumnMode& out);

struct ColumnConfig {
    std::array<ColumnMode, N_COLUMNS> mode{};  // value-initialised: all Real
    // Maintenance suspends the whole display: no frames, no modes, no
    // automatic re-home, nothing moves on its own - while manual commands keep
    // working so a suspect column can be driven by hand.  Persisted, because
    // pulling power mid-repair must not restart a countdown.
    bool maintenance = false;

    bool any(ColumnMode m) const {
        for (const ColumnMode x : mode) {
            if (x == m) return true;
        }
        return false;
    }
    int count(ColumnMode m) const {
        int n = 0;
        for (const ColumnMode x : mode) n += (x == m);
        return n;
    }
    // Columns the frame layer must skip.  Bit i set = column i is excluded.
    uint8_t excluded_mask() const {
        uint8_t m = 0;
        for (int i = 0; i < N_COLUMNS; ++i) {
            if (mode[static_cast<size_t>(i)] == ColumnMode::Disabled) {
                m |= static_cast<uint8_t>(1u << i);
            }
        }
        return m;
    }
};

// A fresh NVS must boot every column REAL and out of maintenance.  Simulated
// motion that could be reached by accident is worse than no simulated motion:
// the whole point is that it can never be mistaken for the real thing.
static_assert(ColumnConfig{}.mode[0] == ColumnMode::Real &&
                  ColumnConfig{}.mode[N_COLUMNS - 1] == ColumnMode::Real,
              "simulated axes must never be the default");
static_assert(!ColumnConfig{}.maintenance, "maintenance must never be the default");

}  // namespace swan
