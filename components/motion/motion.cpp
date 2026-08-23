// IDF shell around the pure axis control core (axis_control.cpp).  This file
// owns everything platform-specific - GPIO, the GPTimer step ISR, the FreeRTOS
// control task, the spinlock, and logging.  It contains NO control logic: the
// FSM, homing, edge verification and mailbox semantics live in the core, which
// the host-side simulated-axis suite drives directly.
//
// Synchronisation contract: docs/MOTION_SYNC.md.  In one line per boundary:
//   ISR <-> tasks     : g_isr[] under g_lock (short critical sections only)
//   tasks -> control  : per-axis request mailbox, posted and drained under g_lock
//   control -> tasks  : AxisCtl atomics, memory_order_relaxed

#include "motion/motion.h"

#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/gpio_bank.h"
#include "motion/axis_control.h"
#include "motion/column_mode.h"
#include "motion/fault_policy.h"
#include "motion/sim_drum.h"
#include "ring/ring.h"

namespace swan {
namespace motion {
namespace {

constexpr const char* TAG = "motion";
constexpr auto RLX = std::memory_order_relaxed;

// ISR-touched data is DRAM-resident: NVS and OTA writes disable the flash
// cache, and a stalled ISR drops steps (spec 5.2, CLAUDE.md).
DRAM_ATTR AxisIsr g_isr[N_COLUMNS];
DRAM_ATTR uint32_t g_step_bit[N_COLUMNS];
DRAM_ATTR uint32_t g_hall_bit[N_COLUMNS];
// XOR mask that turns "Hall asserted" into a 1 bit whatever the polarity, so
// the ISR stays branch-free.
DRAM_ATTR uint32_t g_hall_invert = 0;
// Bit i set = column i drives its real STEP pin.  Simulated and disabled
// columns are masked out here rather than branched on, so the ISR stays the
// same shape whatever the configuration.
DRAM_ATTR uint32_t g_drive_mask = 0;
#if SWAN_SIM_AXES
// Bit i set = column i takes its Hall from the modelled drum below.
DRAM_ATTR uint32_t g_sim_mask = 0;
DRAM_ATTR SimDrum g_sim[N_COLUMNS];
#endif

AxisCtl g_ctl[N_COLUMNS];
Request g_req[N_COLUMNS];  // mailboxes; posted and drained under g_lock

portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;
gptimer_handle_t g_timer = nullptr;
MotionParams g_params;  // guarded by g_lock; snapshotted once per control tick
std::atomic<bool> g_enabled{false};

// ---------------------------------------------------------------------------
// Step ISR - 50 kHz, IRAM.  The per-axis work is the core's two always_inline
// helpers, so the fake host ISR and this one execute the same code.
// ---------------------------------------------------------------------------
bool IRAM_ATTR on_step_alarm(gptimer_handle_t, const gptimer_alarm_event_data_t*, void*) {
    uint32_t step_mask = 0;

    for (int i = 0; i < N_COLUMNS; ++i) {
        if (axis_isr_dda(g_isr[i])) step_mask |= g_step_bit[i];
    }

    step_mask &= g_drive_mask;  // simulated and disabled columns drive nothing
    if (step_mask != 0) gpio_bank_set(step_mask);

    // The Hall read and filter below sit between the STEP rising and falling
    // edges.  A peripheral read plus five filter updates is far more than the
    // TMC2209 minimum 100 ns high time at 240 MHz, so it doubles as the pulse
    // width and costs nothing (spec 5.2).
    const uint32_t active = (gpio_bank_read() ^ g_hall_invert) & HALL_MASK_ALL;

    for (int i = 0; i < N_COLUMNS; ++i) {
#if SWAN_SIM_AXES
        // The ONLY difference between a simulated column and a real one: where
        // the Hall bit comes from.  Everything else - the DDA, the filter, the
        // control core, the FSM, the frame layer above - is the same code.
        const bool raw = ((g_sim_mask >> i) & 1u) != 0
                             ? sim_drum_hall(g_sim[i], g_isr[i].pos_abs)
                             : ((active & g_hall_bit[i]) != 0);
#else
        const bool raw = (active & g_hall_bit[i]) != 0;
#endif
        axis_isr_hall(g_isr[i], raw);
    }

    if (step_mask != 0) gpio_bank_clear(step_mask);
    return false;  // no task woken
}

bool valid_col(int c) { return c >= 0 && c < N_COLUMNS; }

// --- per-column mode ------------------------------------------------------
ColumnConfig g_cols;  // guarded by g_lock for the masks; read freely otherwise

void republish_masks() {
    uint32_t drive = 0;
#if SWAN_SIM_AXES
    uint32_t sim = 0;
#endif
    for (int i = 0; i < N_COLUMNS; ++i) {
        switch (g_cols.mode[static_cast<size_t>(i)]) {
            case ColumnMode::Real:
                drive |= g_step_bit[i];
                break;
            case ColumnMode::Sim:
#if SWAN_SIM_AXES
                sim |= 1u << i;
#endif
                break;
            case ColumnMode::Disabled:
                break;  // neither driven nor simulated: it does not move
        }
    }
    portENTER_CRITICAL(&g_lock);
    g_drive_mask = drive;
#if SWAN_SIM_AXES
    g_sim_mask = sim;
#endif
    portEXIT_CRITICAL(&g_lock);
}

// --- fault escalation (spec 5.4) ------------------------------------------
int faulted_count() {
    int n = 0;
    for (int i = 0; i < N_COLUMNS; ++i) {
        if (g_ctl[i].state.load(RLX) == AxisState::Fault) ++n;
    }
    return n;
}

// Was a column mid open-loop move at alarm speed?  The zero choreography is
// the one time five drums are whirling, and a mechanical problem there is both
// most likely to do damage and least likely to be noticed.
DRAM_ATTR bool g_fast_spin[N_COLUMNS] = {};

bool any_fast_spin() {
    for (int i = 0; i < N_COLUMNS; ++i) {
        if (g_fast_spin[i] && g_ctl[i].state.load(RLX) == AxisState::Moving) return true;
    }
    return false;
}

void apply_escalation(int col, FaultCause cause) {
    const Escalation e = escalate_fault(cause, faulted_count(), any_fast_spin());
    switch (e) {
        case Escalation::ParkColumn:
            ESP_LOGE(TAG, "col %d parked: %s. The other columns keep running.", col,
                     fault_cause_name(cause));
            break;
        case Escalation::StopColumn:
            ESP_LOGE(TAG,
                     "col %d STOPPED - this looks like a JAM, not a sensor fault. Not "
                     "retrying: another homing pass would drive the motor into whatever "
                     "is resisting for 7.5 s. Clear the obstruction, then `home %d`.",
                     col, col);
            break;
        case Escalation::DropEnable:
            // EN is ganged across all five drivers (spec 2.2), so this is the
            // only true de-energize available and it necessarily takes the
            // whole display down.  That is the intended trade: something
            // structural may be wrong and the display is worth less than the
            // mechanism.
            ESP_LOGE(TAG,
                     "col %d fault during %s -> DROPPING EN FOR ALL FIVE COLUMNS. "
                     "Check the mechanism before re-enabling (`en 1`).",
                     col, any_fast_spin() ? "a high-speed spin" : "a multi-column failure");
            enable(false);
            break;
    }
}

// ---------------------------------------------------------------------------
// Control task - 1 kHz.  Snapshot + drain in one critical section, run the
// pure core, write back in another, then log.
// ---------------------------------------------------------------------------
void log_events(int col, const TickEvents& ev, const IsrSnap& in, const AxisCtl& a) {
    if (ev.req_rejected) {
        ESP_LOGW(TAG, "col %d: command rejected in state %s", col,
                 axis_state_name(a.state.load(RLX)));
    }
    if (ev.resync_major) {
        ESP_LOGW(TAG, "col %d hall resync %ld usteps (h2h=%ld)", col,
                 static_cast<long>(a.last_hall_err.load(RLX)),
                 static_cast<long>(a.hall_to_hall.load(RLX)));
    }
    if (ev.fault) {
        ESP_LOGE(TAG, "col %d FAULT: %s (pos=%lld hall=%lld err=%ld)", col, ev.fault_reason,
                 static_cast<long long>(in.pos), static_cast<long long>(in.hall),
                 static_cast<long>(a.last_hall_err.load(RLX)));
    }
    if (ev.rehome) {
        ESP_LOGW(TAG, "col %d re-home attempt %u/%d", col,
                 static_cast<unsigned>(ev.rehome_attempt), REHOME_RETRIES);
    }
    if (ev.gave_up) {
        // Report the attempts that ACTUALLY happened.  A jam latches on the
        // first fault with zero re-homes, and claiming three would send you
        // looking for a thrashing column that never existed - which is the
        // same class of mistake as "fault (re-home 3/3)" reading as
        // still-trying.
        const unsigned tries = a.rehome_attempt.load(RLX);
        if (tries == 0) {
            ESP_LOGE(TAG, "col %d latched FAULT without retrying; `home %d` clears it", col,
                     col);
        } else {
            ESP_LOGE(TAG, "col %d gave up after %u re-home%s; `home %d` clears it", col, tries,
                     tries == 1 ? "" : "s", col);
        }
    }
    if (ev.homed) {
        ESP_LOGI(TAG, "col %d homed at pos=%lld", col, static_cast<long long>(in.pos));
    }
}

void control_tick() {
    // One consistent snapshot of the tunables for the whole tick.
    MotionParams p;
    portENTER_CRITICAL(&g_lock);
    p = g_params;
    portEXIT_CRITICAL(&g_lock);

    for (int i = 0; i < N_COLUMNS; ++i) {
        AxisCtl& a = g_ctl[i];

        // Snapshot and drain together, so a command is never applied against a
        // position newer than the one it was validated with.
        IsrSnap in;
        Request req;
        portENTER_CRITICAL(&g_lock);
        in.pos = g_isr[i].pos_abs;
        in.hall = g_isr[i].hall_abs;
        in.target = g_isr[i].target_abs;
        in.velocity = g_isr[i].velocity;
        in.seq = g_isr[i].hall_seq;
        in.hall_active = g_isr[i].hall_active;
        req = g_req[i];
        g_req[i].kind = ReqKind::None;
        portEXIT_CRITICAL(&g_lock);

        TickEvents ev;
        const IsrWrite w = axis_control_tick(a, in, req, p, ev);

        portENTER_CRITICAL(&g_lock);
        if (w.set_target) g_isr[i].target_abs = w.target;
        g_isr[i].velocity = w.velocity;
        portEXIT_CRITICAL(&g_lock);

        log_events(i, ev, in, a);

        // Escalation is a whole-machine judgement and belongs here, above the
        // per-axis core: only the shell can see how many columns are in
        // trouble and only the shell owns EN.
        if (ev.fault) apply_escalation(i, ev.fault_cause);
        if (a.state.load(RLX) != AxisState::Moving) g_fast_spin[i] = false;
    }
}

void control_task(void*) {
    esp_task_wdt_add(nullptr);
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(1));
        control_tick();
        esp_task_wdt_reset();
    }
}

void post(int col, const Request& r) {
    portENTER_CRITICAL(&g_lock);
    g_req[col] = r;
    portEXIT_CRITICAL(&g_lock);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t init(const MotionParams& p) {
    g_params = p;

    for (int i = 0; i < N_COLUMNS; ++i) {
        g_step_bit[i] = pin_mask(PIN_STEP[i]);
        g_hall_bit[i] = pin_mask(PIN_HALL[i]);
        g_isr[i] = AxisIsr{};
        g_ctl[i].cal_offset.store(normalize_cal(p.cal[i]), RLX);
    }
    g_hall_invert = p.hall_active_low ? HALL_MASK_ALL : 0u;
    republish_masks();

    uint64_t out_mask = 1ULL << PIN_EN;
    for (int i = 0; i < N_COLUMNS; ++i) out_mask |= 1ULL << PIN_STEP[i];

    gpio_config_t out_cfg = {};
    out_cfg.pin_bit_mask = out_mask;
    out_cfg.mode = GPIO_MODE_OUTPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&out_cfg), TAG, "step/en gpio_config");

    gpio_set_level(static_cast<gpio_num_t>(PIN_EN), 1);  // active low: disabled
    gpio_bank_clear(STEP_MASK_ALL);

    uint64_t in_mask = 0;
    for (int i = 0; i < N_COLUMNS; ++i) in_mask |= 1ULL << PIN_HALL[i];

    gpio_config_t in_cfg = {};
    in_cfg.pin_bit_mask = in_mask;
    in_cfg.mode = GPIO_MODE_INPUT;
    // The harness carries an external 10k to 3V3 for the open-collector A3144
    // output (spec 2).  The internal pull-up is belt-and-braces so a missing
    // resistor during bring-up reads as "no magnet" rather than floating.
    in_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&in_cfg), TAG, "hall gpio_config");

    gptimer_config_t tcfg = {};
    tcfg.clk_src = GPTIMER_CLK_SRC_DEFAULT;
    tcfg.direction = GPTIMER_COUNT_UP;
    tcfg.resolution_hz = 10 * 1000 * 1000;  // 0.1 us granularity
    ESP_RETURN_ON_ERROR(gptimer_new_timer(&tcfg, &g_timer), TAG, "gptimer_new_timer");

    gptimer_event_callbacks_t cbs = {};
    cbs.on_alarm = on_step_alarm;
    ESP_RETURN_ON_ERROR(gptimer_register_event_callbacks(g_timer, &cbs, nullptr), TAG, "cb");

    gptimer_alarm_config_t alarm = {};
    alarm.alarm_count = tcfg.resolution_hz / TICK_HZ;  // 200 -> 20 us
    alarm.reload_count = 0;
    alarm.flags.auto_reload_on_alarm = true;
    ESP_RETURN_ON_ERROR(gptimer_set_alarm_action(g_timer, &alarm), TAG, "alarm");
    ESP_RETURN_ON_ERROR(gptimer_enable(g_timer), TAG, "gptimer_enable");
    ESP_RETURN_ON_ERROR(gptimer_start(g_timer), TAG, "gptimer_start");

    // Above lwIP (18) so networking can never delay a control tick, below the
    // WiFi task (23) which has its own hard deadlines.  Step pulses come from
    // the ISR regardless (spec 5.2, CLAUDE.md single-core constraint).
    xTaskCreate(control_task, "swan_motion", 4096, nullptr, 19, nullptr);

    ESP_LOGI(TAG, "%s: step ISR %ld Hz, control %ld Hz, %d columns", BOARD_NAME,
             static_cast<long>(TICK_HZ), static_cast<long>(CONTROL_HZ), N_COLUMNS);
    return ESP_OK;
}

void set_params(const MotionParams& p) {
    portENTER_CRITICAL(&g_lock);
    g_params = p;
    // g_hall_invert is read by the step ISR, so it is published inside the
    // spinlock like everything else in that group.
    g_hall_invert = p.hall_active_low ? HALL_MASK_ALL : 0u;
    portEXIT_CRITICAL(&g_lock);
    for (int i = 0; i < N_COLUMNS; ++i) {
        g_ctl[i].cal_offset.store(normalize_cal(p.cal[i]), RLX);
    }
}

void set_columns(const ColumnConfig& c) {
    ColumnConfig next = c;
#if !SWAN_SIM_AXES
    // The sim is not compiled into this image; refuse to pretend otherwise.
    for (auto& m : next.mode) {
        if (m == ColumnMode::Sim) m = ColumnMode::Real;
    }
#endif
    for (int i = 0; i < N_COLUMNS; ++i) {
        const ColumnMode was = g_cols.mode[static_cast<size_t>(i)];
        const ColumnMode now = next.mode[static_cast<size_t>(i)];
        if (was == now) continue;

        if (now == ColumnMode::Disabled) {
            // Park it on the home slot first, if it can still be moved.  A
            // disabled column is a HOLE in the frame, and a hole showing a
            // stale digit is worse than a hole showing blank: someone reads
            // the wrong time off it with no indication anything is wrong.
            const AxisPublished a = axis_read_published(g_ctl[i]);
            if (a.state == AxisState::Idle && a.index >= 0 && a.hall_valid &&
                a.index != RING_HOME_SLOT) {
                Request r;
                r.kind = ReqKind::Go;
                r.index = RING_HOME_SLOT;
                post(i, r);
                ESP_LOGI(TAG, "col %d disabled; parking it on blank first", i);
            } else {
                ESP_LOGI(TAG, "col %d disabled", i);
            }
        }
#if SWAN_SIM_AXES
        if (now == ColumnMode::Sim) {
            portENTER_CRITICAL(&g_lock);
            g_sim[i] = SimDrum{};
            // A real assembly has no reason to sit on an edge at power-up, and
            // a per-column offset keeps the five from homing in lockstep.
            sim_drum_reset(g_sim[i], g_isr[i].pos_abs, 1234 * (i + 1));
            portEXIT_CRITICAL(&g_lock);
            ESP_LOGW(TAG, "col %d is SIMULATED - no motor is being driven", i);
        }
#endif
    }
    // Maintenance releases EN, and releasing EN is the ONLY true de-energize
    // on this hardware: EN is ganged across all five drivers (spec 2.2, 5.8),
    // so a parked or faulted column still holds standstill current.  BRINGUP
    // tells you to enter maintenance before putting your hands in the
    // mechanism, so this must actually happen and not merely be documented.
    if (next.maintenance != g_cols.maintenance) {
        enable(!next.maintenance);
        ESP_LOGW(TAG, "maintenance %s; EN %s",
                 next.maintenance ? "ON" : "off",
                 next.maintenance ? "RELEASED (all five - it is ganged)" : "asserted");
    }
    g_cols = next;
    republish_masks();
}

ColumnConfig columns() { return g_cols; }

bool any_simulated() { return g_cols.any(ColumnMode::Sim); }

esp_err_t sim_inject_slip(int col, int32_t usteps) {
#if SWAN_SIM_AXES
    if (!valid_col(col)) return ESP_ERR_INVALID_ARG;
    if (g_cols.mode[static_cast<size_t>(col)] != ColumnMode::Sim) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&g_lock);
    sim_drum_slip(g_sim[col], usteps);
    portEXIT_CRITICAL(&g_lock);
    ESP_LOGW(TAG, "col %d SIM: injected %ld usteps of slip", col, static_cast<long>(usteps));
    return ESP_OK;
#else
    (void)col; (void)usteps;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t sim_inject_miss(int col, uint32_t edges) {
#if SWAN_SIM_AXES
    if (!valid_col(col)) return ESP_ERR_INVALID_ARG;
    if (g_cols.mode[static_cast<size_t>(col)] != ColumnMode::Sim) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&g_lock);
    g_sim[col].suppress = edges;
    portEXIT_CRITICAL(&g_lock);
    ESP_LOGW(TAG, "col %d SIM: swallowing the next %lu hall edges", col,
             static_cast<unsigned long>(edges));
    return ESP_OK;
#else
    (void)col; (void)edges;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t sim_clear_faults(int col) {
#if SWAN_SIM_AXES
    for (int i = 0; i < N_COLUMNS; ++i) {
        if (col >= 0 && i != col) continue;
        if (g_cols.mode[static_cast<size_t>(i)] != ColumnMode::Sim) continue;
        portENTER_CRITICAL(&g_lock);
        g_sim[i].suppress = 0;
        sim_drum_reset(g_sim[i], g_isr[i].pos_abs, 0);
        portEXIT_CRITICAL(&g_lock);
    }
    ESP_LOGI(TAG, "sim faults cleared");
    return ESP_OK;
#else
    (void)col;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

MotionParams params() {
    MotionParams p;
    portENTER_CRITICAL(&g_lock);
    p = g_params;
    portEXIT_CRITICAL(&g_lock);
    for (int i = 0; i < N_COLUMNS; ++i) p.cal[i] = g_ctl[i].cal_offset.load(RLX);
    return p;
}

void enable(bool on) {
    g_enabled.store(on, RLX);
    gpio_set_level(static_cast<gpio_num_t>(PIN_EN), on ? 0 : 1);  // active low
}

bool is_enabled() { return g_enabled.load(RLX); }

esp_err_t home(int col) {
    Request r;
    r.kind = ReqKind::Home;

    if (col < 0) {
        for (int i = 0; i < N_COLUMNS; ++i) {
            // A disabled column is never homed - not at boot, not on a
            // re-home-all.  It is excused deliberately, so it must not quietly
            // start moving again.
            if (g_cols.mode[static_cast<size_t>(i)] == ColumnMode::Disabled) continue;
            r.delay_ticks = 1 + i * HOME_STAGGER_MS;  // control ticks == ms
            post(i, r);
        }
        return ESP_OK;
    }
    if (!valid_col(col)) return ESP_ERR_INVALID_ARG;
    if (g_cols.mode[static_cast<size_t>(col)] == ColumnMode::Disabled) {
        return ESP_ERR_INVALID_STATE;
    }
    r.delay_ticks = 1;
    post(col, r);
    return ESP_OK;
}

esp_err_t go(int col, int index) {
    if (!valid_col(col) || !ring_index_valid(index)) return ESP_ERR_INVALID_ARG;

    // Advisory pre-check (the control tick re-validates authoritatively); the
    // two fields are read as a consistent pair via the seqlock.
    const AxisPublished a = axis_read_published(g_ctl[col]);
    if (a.state != AxisState::Idle && a.state != AxisState::Moving) return ESP_ERR_INVALID_STATE;
    if (!a.hall_valid) return ESP_ERR_INVALID_STATE;  // no home reference

    Request r;
    r.kind = ReqKind::Go;
    r.index = index;
    post(col, r);
    return ESP_OK;
}

esp_err_t step_open_loop(int col, int64_t usteps, int32_t flaps_s) {
    if (!valid_col(col) || usteps < 0 || flaps_s <= 0) return ESP_ERR_INVALID_ARG;
    if (g_ctl[col].state.load(RLX) == AxisState::Homing) return ESP_ERR_INVALID_STATE;

    // Remember that this was the alarm-speed whirl: a fault during it drops EN.
    g_fast_spin[col] = flaps_s >= g_params.flaps_s_alarm;

    Request r;
    r.kind = ReqKind::StepOpen;
    r.usteps = usteps;
    r.flaps_s = flaps_s;
    post(col, r);
    return ESP_OK;
}

esp_err_t stop(int col) {
    if (!valid_col(col)) return ESP_ERR_INVALID_ARG;
    Request r;
    r.kind = ReqKind::Stop;
    post(col, r);
    return ESP_OK;
}

esp_err_t set_cal(int col, int32_t usteps) {
    if (!valid_col(col)) return ESP_ERR_INVALID_ARG;
    // A negative nudge is reported as a large positive one and costs most of a
    // revolution to become visible - unavoidable, rotation is one way only.
    const int32_t norm = normalize_cal(usteps);
    g_ctl[col].cal_offset.store(norm, RLX);
    portENTER_CRITICAL(&g_lock);
    g_params.cal[col] = norm;
    portEXIT_CRITICAL(&g_lock);
    return ESP_OK;
}

esp_err_t adjust_cal(int col, int32_t delta) {
    if (!valid_col(col)) return ESP_ERR_INVALID_ARG;
    return set_cal(col, g_ctl[col].cal_offset.load(RLX) + delta);
}

void info(int col, AxisInfo& out) {
    if (!valid_col(col)) return;
    const AxisCtl& a = g_ctl[col];

    // Position, target and velocity must agree with each other, so they come
    // from the spinlock group rather than from separate atomics.
    portENTER_CRITICAL(&g_lock);
    out.pos_abs = g_isr[col].pos_abs;
    out.hall_abs = g_isr[col].hall_abs;
    out.target_abs = g_isr[col].target_abs;
    out.velocity = g_isr[col].velocity;
    out.hall_level = g_isr[col].hall_active;
    portEXIT_CRITICAL(&g_lock);

    // Everything the control tick publishes, as one consistent view: a caller
    // that sees revs == N gets the hall_to_hall of revolution N, and one that
    // sees Idle gets the index that Idle settled on.
    const AxisPublished pub = axis_read_published(a);
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
    out.fault_cause = pub.fault_cause;
    out.mode = g_cols.mode[static_cast<size_t>(col)];
    // Derived from the step count rather than counted per move, so it stays
    // exact across wraps and open-loop stepping.
    out.flips_total =
        static_cast<uint32_t>((out.pos_abs * USTEPS_PER_FLAP_DEN) / USTEPS_PER_FLAP_NUM);
}

bool all_idle() {
    for (int i = 0; i < N_COLUMNS; ++i) {
        const AxisState s = g_ctl[i].state.load(RLX);
        if (s == AxisState::Moving || s == AxisState::Homing) return false;
    }
    return true;
}

}  // namespace motion
}  // namespace swan
