// Five simulated columns for the host dev server (spec 15 phase 3 step a).
//
// The axes are the SAME sim::SimAxis the phase-1.5 suite uses: a modeled drum
// driven by the real step-ISR helpers and the real control tick.  So the web
// UI is exercised against real homing, real edge verification and real
// forward-only motion, months before a drum exists.
//
// NOT internally locked.  The dev server holds one device mutex around the
// simulation advance AND around every HTTP-side call, so the lock order is
// always device mutex -> ModeManager mutex and nothing here needs its own.
#pragma once

#include <array>

#include "frame/frame.h"
#include "motion/axis_control.h"
#include "motion/motion_math.h"
#include "sim_axis.h"
#include "webapi/api.h"

namespace swan {
namespace devserver {

class SimColumns final : public MotionPort, public api::MotionAdmin {
public:
    SimColumns() {
        for (int i = 0; i < N_COLUMNS; ++i) {
            sim::SimAxis& a = ax_[static_cast<size_t>(i)];
            a.params = p_;
            // A different assembly angle and calibration offset per column, so
            // the dev server exercises the edge-anchor reduction rather than
            // the cal == 0 special case (spec 5.3, the bug the 40-agent review
            // found).
            a.drum.start_angle_usteps = 1234 * (i + 1);
            a.ctl.cal_offset.store(normalize_cal(937 * (i + 1)),
                                   std::memory_order_relaxed);
        }
    }

    // Advance the simulation by `ms` of wall time.  One SimAxis tick is 20 us.
    void advance(int64_t ms) {
        const int64_t ticks = ms * (TICK_HZ / 1000);
        for (auto& a : ax_) a.run(ticks);
    }

    // Boot homing, staggered exactly as the firmware does (spec 5.5).
    void home_all() {
        for (int i = 0; i < N_COLUMNS; ++i) {
            ax_[static_cast<size_t>(i)].post_home(
                static_cast<uint32_t>(i) * HOME_STAGGER_MS);
        }
    }

    // Boot gate: every axis has finished its homing pass.
    bool all_homed() const {
        for (const auto& a : ax_) {
            const AxisState st = a.ctl.state.load(std::memory_order_relaxed);
            if (st != AxisState::Idle) return false;
        }
        return true;
    }

    // --- MotionPort (the frame scheduler) ---
    Col col(int i) override {
        const AxisPublished pub = axis_read_published(ax_[static_cast<size_t>(i)].ctl);
        return Col{pub.state, pub.index, pub.dest_index};
    }

    bool go(int i, int index) override {
        if (!valid(i) || !ring_index_valid(index)) return false;
        ax_[static_cast<size_t>(i)].post_go(index);
        return true;
    }

    bool spin(int i, int32_t flaps_s, int seconds) override {
        if (!valid(i)) return false;
        const int64_t usteps =
            (static_cast<int64_t>(flaps_s) * seconds * USTEPS_PER_FLAP_NUM) /
            USTEPS_PER_FLAP_DEN;
        ax_[static_cast<size_t>(i)].post_step_open(usteps, flaps_s);
        return true;
    }

    // --- api::MotionAdmin (the web API) ---
    AxisInfo info(int i) override {
        AxisInfo out{};
        if (!valid(i)) return out;
        const sim::SimAxis& a = ax_[static_cast<size_t>(i)];
        out.pos_abs = a.isr.pos_abs;
        out.hall_abs = a.isr.hall_abs;
        out.target_abs = a.isr.target_abs;
        out.velocity = a.isr.velocity;
        out.hall_level = a.isr.hall_active;

        const AxisPublished pub = axis_read_published(a.ctl);
        out.state = pub.state;
        out.index = pub.index;
        out.dest_index = pub.dest_index;
        out.cal_offset = pub.cal_offset;
        out.hall_valid = pub.hall_valid;
        out.revs = pub.revs;
        out.resync_minor = pub.resync_minor;
        out.resync_major = pub.resync_major;
        out.faults = pub.faults;
        out.last_hall_err = pub.last_hall_err;
        out.hall_to_hall = pub.hall_to_hall;
        out.rehome_attempt = pub.rehome_attempt;
        out.flips_total =
            static_cast<uint32_t>((out.pos_abs * USTEPS_PER_FLAP_DEN) / USTEPS_PER_FLAP_NUM);
        return out;
    }

    MotionParams params() override { return p_; }

    void set_params(const MotionParams& p) override {
        p_ = p;
        for (int i = 0; i < N_COLUMNS; ++i) {
            ax_[static_cast<size_t>(i)].params = p_;
            // cal lives on the axis, not in the params copy the tick reads.
            ax_[static_cast<size_t>(i)].params.cal[i] = p_.cal[i];
        }
    }

    // EN is ganged (spec 2.2): all five or none.
    bool en_ = true;
    bool set_enabled(bool on) override { en_ = on; return true; }
    bool enabled() override { return en_; }
    bool home(int i) override {
        if (i < 0) {
            home_all();
            return true;
        }
        if (!valid(i)) return false;
        ax_[static_cast<size_t>(i)].post_home(1);
        return true;
    }

    bool spin_open_loop(int i, int32_t flaps_s, int seconds) override {
        return spin(i, flaps_s, seconds);
    }

    // The dev server is simulated by construction, so it reports every column
    // as such - the honesty flags mean the same thing here as on the board.
    ColumnConfig columns() override { return cols_; }
    bool set_columns(const ColumnConfig& c) override {
        cols_ = c;
        return true;
    }
    bool sim_inject(int i, std::string_view kind, int32_t value) override {
        if (!valid(i)) return false;
        if (kind == "slip") {
            ax_[static_cast<size_t>(i)].drum.slip_usteps += value;
            return true;
        }
        if (kind == "miss" || kind == "clear") return true;  // modelled on target only
        return false;
    }
    bool sim_available() const override { return true; }

    // Re-seeks, exactly as motion::adjust_cal does on target.  If this did not,
    // the dev server would be the one place where the Calibrate page's nudge
    // appears to work - which is the wrong way round for the surface the UI is
    // developed against.
    api::MotionAdmin::CalOutcome adjust_cal(int i, int32_t delta) override {
        if (!valid(i)) return CalOutcome::BadColumn;
        std::atomic<int32_t>& c = ax_[static_cast<size_t>(i)].ctl.cal_offset;
        const int32_t next = normalize_cal(c.load(std::memory_order_relaxed) + delta);
        c.store(next, std::memory_order_relaxed);
        p_.cal[i] = next;
        const AxisPublished pub = axis_read_published(ax_[static_cast<size_t>(i)].ctl);
        if (!ring_index_valid(pub.index)) return CalOutcome::NotHomed;
        ax_[static_cast<size_t>(i)].post_go(pub.index);
        return CalOutcome::Moved;
    }

private:
    static bool valid(int i) { return i >= 0 && i < N_COLUMNS; }

    std::array<sim::SimAxis, N_COLUMNS> ax_{};
    MotionParams p_{};
    ColumnConfig cols_ = [] {
        ColumnConfig c;
        for (auto& m : c.mode) m = ColumnMode::Sim;
        return c;
    }();
};

}  // namespace devserver
}  // namespace swan
