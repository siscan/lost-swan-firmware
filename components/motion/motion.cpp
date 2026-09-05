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
#include <ctime>

#include "esp_log.h"
#include "journal/journal.h"
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

// The motion layer has no clock of its own by design (it counts microsteps, not
// seconds).  time(nullptr) is the system clock, which is 0-ish until SNTP syncs
// - and the journal renders a 0 as "uptime only" rather than inventing a date.
int64_t journal_utc_s() {
    const time_t t = time(nullptr);
    return t > 1600000000 ? static_cast<int64_t>(t) : 0;
}


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
// Incremented by the step ISR and read by the control tick.  The task watchdog
// covers the 1 kHz control task, but the thing that actually moves the drums is
// this ISR - and if the GPTimer stops, the control task keeps looping and
// feeding the watchdog perfectly happily while nothing moves.  Nothing was
// counting the one component whose death is invisible from every other angle.
DRAM_ATTR volatile uint32_t g_isr_ticks = 0;

bool IRAM_ATTR on_step_alarm(gptimer_handle_t, const gptimer_alarm_event_data_t*, void*) {
    ++g_isr_ticks;
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

// Columns waiting to be parked on blank before their drive bit is cleared.
// See set_columns: posting the park there raced republish_masks and lost.
DRAM_ATTR bool g_park_pending[N_COLUMNS] = {};


// Defined below; the deferred park and the escalation both need it.
void post(int col, const Request& r);

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
                // ... unless it is still parking itself on blank.  The drive
                // bit has to outlive the mode change or the park is a lie.
                if (g_park_pending[i]) drive |= g_step_bit[i];
                break;  // otherwise neither driven nor simulated
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
//
// "Columns in trouble right now", which is not the same as "columns latched in
// Fault", and the difference broke the rule this exists for.  Three corrections,
// all found by walking the maintenance x disabled x simulated matrix:
//
//  1. A RETRYABLE fault stores Unhomed, not Fault, before the shell escalates -
//     so two columns failing together each saw a count of ZERO and both got
//     ParkColumn.  Spec 5.8's "two or more columns faulted -> drop_enable ...
//     two columns failing together is not two coincidences" could only fire
//     after both had exhausted their retries, ~22 s later.  A column with a
//     re-home in flight counts.
//  2. A DISABLED column latched in Fault counted, so the first real fault after
//     a repair dropped EN for all five.  Spec 5.9 says a disabled column is
//     "reported as expected rather than as a fault"; it does not vote.
//  3. SIMULATED columns counted, so injecting two modelled faults on a bench
//     board de-energized the one real column.  The premise for drop_enable is
//     structural - power, the loom, the frame - and a modelled drum is evidence
//     of none of it.  Real columns vote; on a board with NO real columns the
//     simulated ones do, so the rule stays demonstrable where EN means nothing
//     anyway (BRINGUP step 17 exercises exactly that).
int faulted_count() {
    int real_in_trouble = 0, sim_in_trouble = 0, real_columns = 0;
    for (int i = 0; i < N_COLUMNS; ++i) {
        const ColumnMode m = g_cols.mode[static_cast<size_t>(i)];
        if (m == ColumnMode::Disabled) continue;
        if (m == ColumnMode::Real) ++real_columns;
        const bool latched = g_ctl[i].state.load(RLX) == AxisState::Fault;
        const bool recovering = g_ctl[i].rehome_attempt.load(RLX) > 0;
        if (!latched && !recovering) continue;
        if (m == ColumnMode::Real) ++real_in_trouble;
        else ++sim_in_trouble;
    }
    return real_columns > 0 ? real_in_trouble : sim_in_trouble;
}

// Was a column mid open-loop move at alarm speed?  The zero choreography is
// the one time five drums are whirling, and a mechanical problem there is both
// most likely to do damage and least likely to be noticed.
DRAM_ATTR bool g_fast_spin[N_COLUMNS] = {};

// `col` is the column that has just faulted.  It is treated as still spinning
// even though its state has already moved to Fault or Unhomed - it excluded
// ITSELF from this test otherwise, so "a fault during a high-speed spin drops
// EN" could not fire for the single spinning column that faulted, which is the
// case spec 5.8 describes.
bool any_fast_spin(int col) {
    for (int i = 0; i < N_COLUMNS; ++i) {
        if (!g_fast_spin[i]) continue;
        if (i == col) return true;
        if (g_ctl[i].state.load(RLX) == AxisState::Moving) return true;
    }
    return false;
}

void apply_escalation(int col, FaultCause cause) {
    const Escalation e = escalate_fault(cause, faulted_count(), any_fast_spin(col));
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
                     col, any_fast_spin(col) ? "a high-speed spin" : "a multi-column failure");
            // enable(false) stops all five itself now - see the rule on
            // enable().  Releasing EN used to write only the pin, so the axes
            // carried on stepping in software into dead drivers, completed
            // their moves and published indices the drums never reached.
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
        // Into the permanent journal too: a fault at 3 a.m. that recovered by
        // itself is exactly the thing the console has already forgotten by the
        // time anybody looks.  A zero-timeout queue send from the control
        // task - it never waits on the filesystem.
        journal::note_fault(journal_utc_s(), col, ev.fault_reason);
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
        // Only a homing that ENDED a fault is a recovery; a boot home is not.
        // The count comes off the EVENT, not off the axis: the axis clears it
        // when the hall edge lands, which is before this event exists, so
        // reading it here always saw zero and no recovery was ever journalled.
        if (ev.recovered_after > 0) {
            journal::note_recover(journal_utc_s(), col, ev.recovered_after);
        }
    }
}

// Finishes a deferred park (see set_columns) and drops the drive bit once the
// column is actually sitting on blank.  Bounded, because a column that cannot
// reach blank must not keep its driver energized for ever.
void tick_park_pending() {
    static uint16_t waited[N_COLUMNS] = {};
    bool changed = false;
    for (int i = 0; i < N_COLUMNS; ++i) {
        if (!g_park_pending[i]) {
            waited[i] = 0;
            continue;
        }
        const AxisPublished a = axis_read_published(g_ctl[i]);
        if (a.state == AxisState::Idle && a.index == RING_HOME_SLOT) {
            g_park_pending[i] = false;
            waited[i] = 0;
            changed = true;
            ESP_LOGI(TAG, "col %d parked on blank; it is now out of every frame", i);
        } else if (a.state == AxisState::Idle) {
            Request r;
            r.kind = ReqKind::Go;
            r.index = RING_HOME_SLOT;
            post(i, r);
        }
        // 10 s is four times the worst legitimate park (a 49-flip wrap at the
        // homing speed); past that something is wrong and holding the driver
        // on helps nobody.
        // 30 s, and it STOPS the axis first.  Clearing the drive bit under a
        // moving axis is the very thing the deferred park exists to prevent:
        // the DDA keeps advancing, the move "completes" in software and the
        // column reports itself parked on blank while the drum stands wherever
        // the bit was pulled.  The old 10 s bound was also derived from the
        // homing speed, and a park is a Go at flaps_s_normal - which Settings
        // can set as low as 1 flap/s, making a legitimate 49-flip park take 49 s.
        if (++waited[i] > 30000) {
            Request r;
            r.kind = ReqKind::Stop;
            // UNKNOWN, not blank.  A plain Stop publishes where the move was
            // HEADED, and this move was headed for the home slot - so bailing
            // out reported the column parked on blank, which is the exact lie
            // the deferred park exists to prevent.  It did not get there; say
            // so.
            r.invalidate_index = true;
            post(i, r);
            g_park_pending[i] = false;
            waited[i] = 0;
            changed = true;
            ESP_LOGW(TAG, "col %d did not reach blank in 30 s; stopped where it stands "
                          "and disabled there", i);
        }
    }
    if (changed) republish_masks();
}

void control_tick() {
    // One consistent snapshot of the tunables for the whole tick.
    MotionParams p;
    portENTER_CRITICAL(&g_lock);
    p = g_params;
    portEXIT_CRITICAL(&g_lock);

    tick_park_pending();

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

std::atomic<uint32_t> g_control_ticks{0};
std::atomic<bool> g_step_isr_alive{true};
std::atomic<uint32_t> g_isr_stalls{0};

// The step ISR should tick ~50 times per control tick.  Zero for this many
// consecutive control ticks (200 ms) means the timer has stopped, which is not
// something any other check can see: the control task is still looping, the
// watchdog is still fed, the web UI still answers, and the drums are still.
constexpr uint32_t ISR_DEAD_TICKS = 200;

void control_task(void*) {
    // Checked, unlike before.  The modes task does the same call under
    // ESP_ERROR_CHECK; the two most important tasks in the firmware were using
    // opposite error policies for it, and a silent failure here means the 1 kHz
    // tick runs unwatched for ever with nothing saying so.
    const esp_err_t werr = esp_task_wdt_add(nullptr);
    if (werr != ESP_OK) {
        ESP_LOGE(TAG, "the control task is NOT watched by the task watchdog (%s)",
                 esp_err_to_name(werr));
    }
    TickType_t last = xTaskGetTickCount();
    uint32_t last_isr = g_isr_ticks;
    uint32_t idle_for = 0;
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(1));
        control_tick();

        const uint32_t now_isr = g_isr_ticks;
        if (now_isr == last_isr) {
            if (++idle_for == ISR_DEAD_TICKS) {
                // Report rather than reboot.  A false positive that panicked a
                // wall display would be worse than the fault it is looking for,
                // and this is visible in `stats`, in Diagnostics and in the
                // state payload the moment it happens.
                g_step_isr_alive.store(false, std::memory_order_relaxed);
                g_isr_stalls.fetch_add(1, std::memory_order_relaxed);
                ESP_LOGE(TAG, "*** THE STEP TIMER HAS STOPPED - no ISR ticks for %u ms. "
                              "The columns cannot move; nothing else will notice this. ***",
                         static_cast<unsigned>(ISR_DEAD_TICKS));
            }
        } else {
            if (!g_step_isr_alive.load(std::memory_order_relaxed)) {
                ESP_LOGW(TAG, "the step timer is ticking again");
                g_step_isr_alive.store(true, std::memory_order_relaxed);
            }
            idle_for = 0;
            last_isr = now_isr;
        }

        g_control_ticks.fetch_add(1, std::memory_order_relaxed);
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

// The ganged DIR level.  Plain relaxed atomic: it is written by the control
// task or a command and read by whoever last needs to drive the pin, and there
// is nothing to pair it with, so it is not part of the AxisCtl seqlock
// (docs/MOTION_SYNC.md boundary 3).
DRAM_ATTR std::atomic<bool> g_dir_invert{false};

void apply_dir_level() {
    if (!HAS_DIR_GPIO) return;
    gpio_set_level(static_cast<gpio_num_t>(PIN_DIR), g_dir_invert.load(RLX) ? 1 : 0);
}

esp_err_t init(const MotionParams& p) {
    g_params = p;

    for (int i = 0; i < N_COLUMNS; ++i) {
        g_step_bit[i] = pin_mask(PIN_STEP[i]);
        g_hall_bit[i] = pin_mask(PIN_HALL[i]);
        g_isr[i] = AxisIsr{};
        g_ctl[i].cal_offset.store(normalize_cal(p.cal[i]), RLX);
    }
    g_hall_invert = p.hall_active_low ? HALL_MASK_ALL : 0u;
    g_dir_invert.store(p.dir_invert, RLX);
    republish_masks();

    uint64_t out_mask = 1ULL << PIN_EN;
    for (int i = 0; i < N_COLUMNS; ++i) out_mask |= 1ULL << PIN_STEP[i];
    // DIR is ganged like EN and, on a board that has a pin for it, is driven
    // before anything can step.  It is a plain level, not a per-step signal:
    // the TMC2209 samples it on the STEP edge, so it only has to be settled
    // before the first pulse and never changes inside a move.
    if (HAS_DIR_GPIO) out_mask |= 1ULL << PIN_DIR;

    gpio_config_t out_cfg = {};
    out_cfg.pin_bit_mask = out_mask;
    out_cfg.mode = GPIO_MODE_OUTPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&out_cfg), TAG, "step/en gpio_config");

    gpio_set_level(static_cast<gpio_num_t>(PIN_EN), 1);  // active low: disabled
    apply_dir_level();
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
    // DIR takes effect immediately, which is the point: bench step 3 is a
    // person watching a drum and typing `dir` until it turns the descending
    // way.  The pin is a level the driver samples on the next STEP edge, so
    // changing it between moves is safe; changing it DURING one would reverse
    // mid-move, which is why the dispatcher refuses it on a moving axis.
    g_dir_invert.store(p.dir_invert, RLX);
    apply_dir_level();
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
            //
            // The park is DEFERRED, not posted here: clearing this column's
            // drive bit a few microseconds later (republish_masks, below) beat
            // the 1 kHz tick to the mailbox, so the move ran with STEP already
            // masked off - the axis advanced pos_abs, reported Idle at the home
            // slot, and the drum never moved.  The control tick performs the
            // park while the column is still driven and clears the bit after.
            const AxisPublished a = axis_read_published(g_ctl[i]);
            if (a.state == AxisState::Idle && a.index >= 0 && a.hall_valid &&
                a.index != RING_HOME_SLOT) {
                g_park_pending[i] = true;
                ESP_LOGI(TAG, "col %d disabled; parking it on blank first", i);
            } else if (a.state == AxisState::Moving || a.state == AxisState::Homing) {
                // Mid-move: stop it where it is rather than let it finish with
                // STEP masked off and publish a position it never reached.
                Request r;
                r.kind = ReqKind::Stop;
                post(i, r);
                ESP_LOGI(TAG, "col %d disabled mid-move; stopped where it is", i);
            } else {
                ESP_LOGI(TAG, "col %d disabled", i);
            }
        }
        if (was == ColumnMode::Disabled && now != ColumnMode::Disabled) {
            // A homing pass with EN released steps 1.2 revolutions into dead
            // drivers, sees no edge and latches a no_hall FAULT - a fault
            // manufactured by the act of re-enabling, on a column that is fine.
            //
            // THE TEST IS "ARE THE DRIVERS ENERGIZED", not "is maintenance on".
            // It used to be the latter, which covered exactly one of the three
            // ways EN goes down and missed the two that matter most: after an
            // escalation, and after a manual `en 0`.  Re-enabling a column in
            // either state manufactured the fault this comment describes.
            // Asserting EN re-homes everything anyway, so nothing is lost by
            // waiting for it.
            g_park_pending[i] = false;   // it is not parking any more
            if (!g_enabled.load(RLX) || next.maintenance) {
                ESP_LOGI(TAG, "col %d re-enabled; it will home when the drivers are", i);
                continue;
            }
            // Leaving disabled has to HOME.  A disabled column is never homed -
            // not at boot, not on a re-home-all - so after a reboot it comes
            // back Unhomed with no hall reference, motion::go refuses it on
            // every render, and the frame scheduler's convergence pass only
            // acts on Idle columns.  It never closed the hole; it just stopped
            // being reported as a hole.
            Request r;
            r.kind = ReqKind::Home;
            r.delay_ticks = 1;
            post(i, r);
            ESP_LOGI(TAG, "col %d re-enabled; homing it", i);
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

bool step_isr_alive() { return g_step_isr_alive.load(std::memory_order_relaxed); }
uint32_t step_isr_stalls() { return g_isr_stalls.load(std::memory_order_relaxed); }
uint32_t step_isr_ticks() { return g_isr_ticks; }

uint32_t control_ticks() { return g_control_ticks.load(std::memory_order_relaxed); }

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


// ONE RULE, and it replaced a flag plus three special cases that between them
// carried two criticals (the scoped re-sweep, 2026-08-24):
//
//     releasing EN STOPS every axis.  asserting EN RE-HOMES every
//     non-disabled axis, unless maintenance is on.
//
// No memory of *why* EN went down, which is what the previous version got
// wrong: it remembered "an escalation dropped this" in a plain bool that ANY
// enable() call cleared - so `en 0` on an already-dead display silently
// cancelled the recovery re-home and left it powered and permanently still.
// The rule below cannot have that bug because it has nothing to forget.
//
// The two halves are the same argument from opposite ends.  De-energized drums
// coast and are pushed; an axis that keeps running its DDA into dead drivers
// completes the move in software and publishes a face the drum never reached.
// So: stop on the way down, and on the way up distrust the position, because
// nothing watched the drums while they were unpowered - whether they were
// unpowered by an escalation, by a person, or by a repair.
//
// MAINTENANCE IS THE ONE EXEMPTION, and it is spec 5.9 rather than a
// convenience: a boot in maintenance deliberately does not home, because the
// operator's hands are in the mechanism.  Energizing during a repair gives them
// powered, stationary drums - which is exactly what the Calibrate page wants.
// Leaving maintenance calls enable(true) with the flag already cleared, so the
// re-home that spec 5.9 promises still happens, from this same line.
void enable(bool on) {
    // Serialised: this is called from the 1 kHz control task (escalation), from
    // httpd, from the CLI and from the modes task, and it both writes a pin and
    // posts to five mailboxes.  Two callers interleaving could leave the pin
    // and g_enabled disagreeing - a display reporting itself de-energized while
    // the drivers are live.
    portENTER_CRITICAL(&g_lock);
    const bool was = g_enabled.load(RLX);
    g_enabled.store(on, RLX);
    gpio_set_level(static_cast<gpio_num_t>(PIN_EN), on ? 0 : 1);  // active low
    const bool changed = (was != on);
    portEXIT_CRITICAL(&g_lock);
    if (!changed) return;

    if (!on) {
        ESP_LOGW(TAG, "EN released for all five - stopping every axis");
        Request r;
        r.kind = ReqKind::Stop;
        for (int i = 0; i < N_COLUMNS; ++i) post(i, r);
        return;
    }

    if (g_cols.maintenance) {
        ESP_LOGI(TAG, "EN asserted, maintenance on - NOT homing (spec 5.9); "
                      "the drums are yours");
        return;
    }
    ESP_LOGW(TAG, "EN asserted - re-homing every column, because nothing watched "
                  "the drums while they were unpowered");
    Request r;
    r.kind = ReqKind::Home;
    for (int i = 0; i < N_COLUMNS; ++i) {
        if (g_cols.mode[static_cast<size_t>(i)] == ColumnMode::Disabled) continue;
        r.delay_ticks = 1 + i * HOME_STAGGER_MS;
        post(i, r);
    }
}

bool is_enabled() { return g_enabled.load(RLX); }

esp_err_t home(int col) {
    Request r;
    r.kind = ReqKind::Home;

    if (col < 0) {
        int posted = 0;
        for (int i = 0; i < N_COLUMNS; ++i) {
            // A disabled column is never homed - not at boot, not on a
            // re-home-all.  It is excused deliberately, so it must not quietly
            // start moving again.
            if (g_cols.mode[static_cast<size_t>(i)] == ColumnMode::Disabled) continue;
            r.delay_ticks = 1 + i * HOME_STAGGER_MS;  // control ticks == ms
            post(i, r);
            ++posted;
        }
        // With every column disabled this posted nothing, and used to say ok.
        return posted > 0 ? ESP_OK : ESP_ERR_INVALID_STATE;
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
    // A disabled column is excused from everything (spec 5.9) - home() has
    // always said so and this did not, so `spin` on a disabled column was
    // accepted, ran the DDA for its full duration and left the axis reporting a
    // position for a drum nobody is driving.  The frame layer then skips it, so
    // nothing ever corrects the lie.
    if (g_cols.mode[static_cast<size_t>(col)] == ColumnMode::Disabled) {
        return ESP_ERR_INVALID_STATE;
    }

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
    const esp_err_t err = set_cal(col, g_ctl[col].cal_offset.load(RLX) + delta);
    if (err != ESP_OK) return err;

    // Re-seek the index the column is already showing, so the nudge MOVES the
    // drum.  This lives here rather than in each caller because it did not:
    // the CLI re-seeked and the web path did not, so the Calibrate page's
    // +-1/+-10 buttons changed a number and left the card exactly where it was
    // - on the one page whose entire purpose is "nudge until the blank card
    // hangs flat against the bezel lip".  A caller cannot be trusted to
    // remember; adjusting the offset IS a move.
    //
    // ESP_ERR_INVALID_STATE is returned deliberately when the column has no
    // home reference: the offset HAS been applied, but nothing can move until
    // the column is homed, and the caller must be able to say so rather than
    // report a nudge that did nothing.
    const AxisPublished a = axis_read_published(g_ctl[col]);
    if (!ring_index_valid(a.index)) return ESP_ERR_INVALID_STATE;
    return go(col, a.index);
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
