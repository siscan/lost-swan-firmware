// Fakes shared by the frame and mode suites.
#pragma once

#include <vector>

#include "frame/frame.h"
#include "modes/mode_manager.h"
#include "ring/ring.h"
#include "timesvc/time_source.h"

namespace swan {
namespace testfakes {

struct FakePort final : MotionPort {
    struct C {
        AxisState state = AxisState::Idle;
        int index = 0;
        int dest = 0;
    };
    std::array<C, N_COLUMNS> cols{};

    struct GoRec {
        int64_t at_ms;
        int col;
        int index;
    };
    struct SpinRec {
        int64_t at_ms;
        int col;
        int32_t flaps_s;
        int seconds;
    };
    std::vector<GoRec> gos;
    std::vector<SpinRec> spins;

    int64_t now_ms = 0;    // the test advances this alongside its clock
    bool accept = true;    // go() return value
    bool instant = true;   // true: go settles immediately; false: Moving until finish()

    Col col(int i) override {
        const C& c = cols[static_cast<size_t>(i)];
        return Col{c.state, c.index, c.dest};
    }

    bool go(int i, int index) override {
        gos.push_back({now_ms, i, index});
        if (!accept) return false;
        C& c = cols[static_cast<size_t>(i)];
        if (instant) {
            c.index = index;
            c.state = AxisState::Idle;
        } else {
            c.dest = index;
            c.state = AxisState::Moving;
        }
        return true;
    }

    bool spin(int i, int32_t flaps_s, int seconds) override {
        spins.push_back({now_ms, i, flaps_s, seconds});
        C& c = cols[static_cast<size_t>(i)];
        // Mirrors the real axis: during an open-loop move the published index
        // stays at the last settled value; it becomes unknown on ARRIVAL
        // (index <- dest_index == RING_INVALID).
        c.state = AxisState::Moving;
        c.dest = RING_INVALID;
        if (instant) {
            c.state = AxisState::Idle;
            c.index = RING_INVALID;
        }
        return true;
    }

    void finish_moves() {
        for (auto& c : cols) {
            if (c.state == AxisState::Moving) {
                c.state = AxisState::Idle;
                c.index = c.dest;
            }
        }
    }

    int gos_for(int col_i) const {
        int n = 0;
        for (const auto& g : gos) n += (g.col == col_i);
        return n;
    }
};

struct FakeTime final : TimeSource {
    int64_t utc_ms = 0;
    bool is_valid = true;
    int64_t now_utc() override { return utc_ms / 1000; }
    bool valid() override { return is_valid; }
};

struct FakeStore final : CountdownStore {
    CdPersist stored{};
    bool have = false;
    int saves = 0;
    bool load(CdPersist& out) override {
        if (!have) return false;
        out = stored;
        return true;
    }
    void save(const CdPersist& s) override {
        stored = s;
        have = true;
        ++saves;
    }
};

struct FakeCues final : CueSink {
    struct Rec {
        Cue cue;
        int64_t at_ms;
    };
    std::vector<Rec> recs;
    int64_t now_ms = 0;
    void on_cue(Cue c) override { recs.push_back({c, now_ms}); }
    bool fired(Cue c) const {
        for (const auto& r : recs) {
            if (r.cue == c) return true;
        }
        return false;
    }
    int64_t at(Cue c) const {
        for (const auto& r : recs) {
            if (r.cue == c) return r.at_ms;
        }
        return -1;
    }
};

}  // namespace testfakes
}  // namespace swan
