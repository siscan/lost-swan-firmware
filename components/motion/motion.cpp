#include "motion/motion.h"

#include <atomic>

#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/gpio_bank.h"
#include "ring/ring.h"

namespace swan {
namespace motion {
namespace {

constexpr const char* TAG = "motion";

constexpr int REHOME_RETRIES = 3;
constexpr uint32_t HOME_STAGGER_MS = 250;  // spec 5.5, limits inrush
// 1.2 revolutions without an edge is a homing timeout (spec 5.5).
constexpr int64_t HOME_LIMIT = (USTEPS_PER_SPOOL_REV_NOMINAL * 6) / 5;

enum class HomePhase : unsigned char { None, Release, Seek, Settle };

// ===========================================================================
// Cross-task state handoff.  Three boundaries, each with one explicit
// mechanism.  No field is shared by store-ordering convention.
//
//   (1) step ISR  <-> control task
//       g_isr[] is plain data in DRAM, guarded by g_lock.  The ISR cannot take
//       a lock, so the task side takes the spinlock, which disables interrupts
//       for its duration and therefore excludes the ISR.  Every task-side read
//       or write of g_isr[] happens inside portENTER/EXIT_CRITICAL.
//
//   (2) any task  ->  control task   (commands)
//       A per-axis request mailbox, written under g_lock and drained by the
//       control tick under the same lock.  The control task is the only writer
//       of the axis FSM, so a command can never race a state transition.
//
//   (3) control task -> any task     (published state and diagnostics)
//       std::atomic with explicit memory_order_relaxed.  Relaxed is correct
//       here: every one of these is independently meaningful and none of them
//       publishes ownership of other memory.  Anything needing a *consistent*
//       multi-field view (position vs target vs velocity) goes through (1)
//       instead, which is why info() takes the spinlock for that group.
// ===========================================================================

// (1) Everything the step ISR touches.  Plain data, DRAM-resident: NVS and OTA
// writes disable the flash cache and a stalled ISR drops steps (spec 5.2).
// Deliberately a separate struct from Axis so the IRAM/DRAM boundary is visible
// rather than a comment on a mixed struct.
struct AxisIsr {
    int64_t pos_abs;
    int64_t target_abs;
    int64_t hall_abs;
    int32_t velocity;
    uint32_t accum;
    uint32_t hall_seq;
    uint8_t hall_hist;
    bool hall_active;
};

DRAM_ATTR AxisIsr g_isr[N_COLUMNS];
DRAM_ATTR uint32_t g_step_bit[N_COLUMNS];
DRAM_ATTR uint32_t g_hall_bit[N_COLUMNS];
// XOR mask that turns "Hall asserted" into a 1 bit whatever the polarity, so
// the ISR stays branch-free.
DRAM_ATTR uint32_t g_hall_invert = 0;

// (2) Command mailbox.  One slot per axis: a newer command replaces an
// undrained older one, which is exactly the frame semantics in spec 6 ("a new
// frame while moving simply replaces targets").
enum class ReqKind : unsigned char { None, Home, Go, StepOpen, Stop };

struct Request {
    ReqKind kind = ReqKind::None;
    int index = 0;             // Go
    int64_t usteps = 0;        // StepOpen
    int32_t flaps_s = 0;       // StepOpen
    uint32_t delay_ticks = 0;  // Home
};

struct Axis {
    // (2) written under g_lock by any task, drained under g_lock by control.
    Request req;

    // (3) published by the control task, read by anyone.
    std::atomic<AxisState> state{AxisState::Unhomed};
    std::atomic<int> index{RING_INVALID};
    std::atomic<int> dest_index{RING_INVALID};
    std::atomic<bool> hall_valid{false};
    std::atomic<uint32_t> revs{0};
    std::atomic<uint32_t> resync_minor{0};
    std::atomic<uint32_t> resync_major{0};
    std::atomic<uint32_t> faults{0};
    std::atomic<int32_t> last_hall_err{0};
    std::atomic<int32_t> hall_to_hall{0};

    // Written by the calibration API, read by the control task.  A single
    // 32-bit value with no companion invariant, so an atomic is the whole
    // mechanism - live nudging during a move is intentional (spec 5.6).
    std::atomic<int32_t> cal_offset{0};

    // Control-task private.  Nothing else may touch these.
    HomePhase home_phase = HomePhase::None;
    int32_t v_max = 0;
    int64_t hall_prev = 0;
    uint32_t seq_seen = 0;
    uint32_t home_delay = 0;
    uint8_t rehome_retries = 0;
};

Axis g_ax[N_COLUMNS];

portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;
gptimer_handle_t g_timer = nullptr;
MotionParams g_params;  // guarded by g_lock; snapshotted once per control tick
std::atomic<bool> g_enabled{false};

constexpr auto RLX = std::memory_order_relaxed;

static_assert(std::atomic<int32_t>::is_always_lock_free,
              "32-bit atomics must be lock-free; a libatomic call would not be "
              "safe next to the step ISR");
static_assert(std::atomic<AxisState>::is_always_lock_free, "AxisState must be lock-free");

// ---------------------------------------------------------------------------
// Step ISR - 50 kHz, IRAM, touches only g_isr[] and the DRAM masks.
// ---------------------------------------------------------------------------
bool IRAM_ATTR on_step_alarm(gptimer_handle_t, const gptimer_alarm_event_data_t*, void*) {
    uint32_t step_mask = 0;

    for (int i = 0; i < N_COLUMNS; ++i) {
        AxisIsr& a = g_isr[i];
        const int32_t v = a.velocity;
        if (v > 0 && a.pos_abs < a.target_abs) {
            if (dda_tick(a.accum, v)) {
                step_mask |= g_step_bit[i];
                ++a.pos_abs;
            }
        }
    }

    if (step_mask != 0) gpio_bank_set(step_mask);

    // The Hall read and filter below sit between the STEP rising and falling
    // edges.  A peripheral read plus five filter updates is far more than the
    // TMC2209 minimum 100 ns high time at 240 MHz, so it doubles as the pulse
    // width and costs nothing (spec 5.2).
    const uint32_t active = (gpio_bank_read() ^ g_hall_invert) & HALL_MASK_ALL;

    for (int i = 0; i < N_COLUMNS; ++i) {
        AxisIsr& a = g_isr[i];
        const uint8_t raw = (active & g_hall_bit[i]) ? 1u : 0u;
        const uint8_t h = static_cast<uint8_t>(((a.hall_hist << 1) | raw) & 0x7u);
        a.hall_hist = h;

        // 2-of-3 majority.  Worst case it delays the edge by two ticks = 40 us,
        // which at alarm speed is under a quarter of a microstep.
        const unsigned ones = (h & 1u) + ((h >> 1) & 1u) + ((h >> 2) & 1u);
        const bool level = ones >= 2u;

        if (level && !a.hall_active) {
            // Latched here, inside the ISR, so the edge and the step count can
            // never disagree (spec 5.2).
            a.hall_abs = a.pos_abs;
            ++a.hall_seq;
        }
        a.hall_active = level;
    }

    if (step_mask != 0) gpio_bank_clear(step_mask);
    return false;  // no task woken
}

// ---------------------------------------------------------------------------
// Control tick (task context, 1 kHz)
// ---------------------------------------------------------------------------
void set_target_locked(int col, int64_t t) {
    portENTER_CRITICAL(&g_lock);
    g_isr[col].target_abs = t;
    portEXIT_CRITICAL(&g_lock);
}

void set_velocity_locked(int col, int32_t v) {
    portENTER_CRITICAL(&g_lock);
    g_isr[col].velocity = v;
    portEXIT_CRITICAL(&g_lock);
}

void begin_home(int col, Axis& a, int64_t pos, bool hall_now, const MotionParams& p) {
    a.dest_index.store(RING_HOME_SLOT, RLX);
    a.index.store(RING_INVALID, RLX);
    a.hall_valid.store(false, RLX);  // first edge of a pass has no predecessor
    a.v_max = flaps_s_to_usteps_s(p.flaps_s_home);
    // If we are sitting inside the magnet zone, step out of it first (spec 5.5).
    a.home_phase = hall_now ? HomePhase::Release : HomePhase::Seek;
    set_target_locked(col, pos + HOME_LIMIT);
    a.state.store(AxisState::Homing, RLX);
}

void enter_fault(int col, Axis& a, const char* why) {
    set_velocity_locked(col, 0);
    a.faults.fetch_add(1, RLX);

    int64_t pos, hall;
    portENTER_CRITICAL(&g_lock);
    pos = g_isr[col].pos_abs;
    hall = g_isr[col].hall_abs;
    portEXIT_CRITICAL(&g_lock);

    ESP_LOGE(TAG, "col %d FAULT: %s (pos=%lld hall=%lld err=%ld)", col, why,
             static_cast<long long>(pos), static_cast<long long>(hall),
             static_cast<long>(a.last_hall_err.load(RLX)));

    if (a.rehome_retries < REHOME_RETRIES) {
        ++a.rehome_retries;
        ESP_LOGW(TAG, "col %d re-home attempt %u/%d", col,
                 static_cast<unsigned>(a.rehome_retries), REHOME_RETRIES);
        a.home_delay = 1;
        a.state.store(AxisState::Unhomed, RLX);
    } else {
        // Phase 6 applies the fault display policy (Q5: park on blank, others
        // continue) and publishes over MQTT.  Phase 2 resumes the frame after a
        // successful re-home.
        a.state.store(AxisState::Fault, RLX);
        ESP_LOGE(TAG, "col %d gave up after %d re-homes; `home %d` clears it", col,
                 REHOME_RETRIES, col);
    }
}

void on_hall_edge(int col, Axis& a, int64_t hall, int64_t pos, const MotionParams& p) {
    if (a.hall_valid.load(RLX)) {
        const int64_t err = edge_error(a.hall_prev, hall);
        a.hall_to_hall.store(static_cast<int32_t>(hall - a.hall_prev), RLX);
        a.last_hall_err.store(static_cast<int32_t>(err), RLX);
        a.revs.fetch_add(1, RLX);

        const EdgeTolerances tol{p.hall_tol, static_cast<int32_t>(ring_target_usteps(1))};
        switch (classify_edge_error(err, tol)) {
            case EdgeVerdict::Minor:
                a.resync_minor.fetch_add(1, RLX);
                break;
            case EdgeVerdict::Major:
                a.resync_major.fetch_add(1, RLX);
                ESP_LOGW(TAG, "col %d hall resync %ld usteps (h2h=%ld)", col,
                         static_cast<long>(err),
                         static_cast<long>(a.hall_to_hall.load(RLX)));
                break;
            case EdgeVerdict::Fault:
                enter_fault(col, a, "hall edge off by more than one flap");
                return;
        }
    }

    a.hall_prev = hall;
    a.hall_valid.store(true, RLX);

    const int32_t cal = a.cal_offset.load(RLX);
    const AxisState st = a.state.load(RLX);

    if (st == AxisState::Homing && a.home_phase == HomePhase::Seek) {
        a.home_phase = HomePhase::Settle;
        a.rehome_retries = 0;
        // Forward-clamped, NOT plan_target.  By the time the control tick sees
        // the edge, pos has already crept a ustep or two past hall, so with the
        // common cal_offset of 0 plan_target would wrap a whole extra
        // revolution and leave pos referenced to the previous edge.
        set_target_locked(col, retarget_on_edge(hall, cal, pos, RING_HOME_SLOT));
        return;
    }

    // A move that crossed home: rebase on the edge that just landed, so the
    // 0.42 ustep/rev residue is absorbed instead of accumulating (spec 5.3).
    const int dest = a.dest_index.load(RLX);
    if (st == AxisState::Moving && ring_index_valid(dest)) {
        set_target_locked(col, retarget_on_edge(hall, cal, pos, dest));
    }
}

// Applies one drained command.  Runs on the control task, which is the only
// writer of the FSM, so validation here is authoritative - the checks in the
// public API are advisory, for immediate CLI feedback only.
void apply_request(int col, Axis& a, const Request& r, int64_t pos, const MotionParams& p) {
    switch (r.kind) {
        case ReqKind::Home:
            a.rehome_retries = 0;
            a.home_delay = r.delay_ticks;
            a.state.store(AxisState::Unhomed, RLX);
            break;

        case ReqKind::Go: {
            const AxisState s = a.state.load(RLX);
            if ((s != AxisState::Idle && s != AxisState::Moving) || !a.hall_valid.load(RLX)) {
                ESP_LOGW(TAG, "col %d: go rejected in state %s", col, axis_state_name(s));
                break;
            }
            a.dest_index.store(r.index, RLX);
            a.v_max = flaps_s_to_usteps_s(p.flaps_s_normal);
            portENTER_CRITICAL(&g_lock);
            g_isr[col].target_abs = plan_target(g_isr[col].hall_abs, a.cal_offset.load(RLX),
                                                g_isr[col].pos_abs, r.index);
            portEXIT_CRITICAL(&g_lock);
            a.state.store(AxisState::Moving, RLX);
            break;
        }

        case ReqKind::StepOpen: {
            if (a.state.load(RLX) == AxisState::Homing) {
                ESP_LOGW(TAG, "col %d: step rejected while homing", col);
                break;
            }
            a.dest_index.store(RING_INVALID, RLX);
            a.v_max = flaps_s_to_usteps_s(r.flaps_s);
            portENTER_CRITICAL(&g_lock);
            g_isr[col].target_abs = g_isr[col].pos_abs + r.usteps;
            portEXIT_CRITICAL(&g_lock);
            a.state.store(AxisState::Moving, RLX);
            break;
        }

        case ReqKind::Stop:
            portENTER_CRITICAL(&g_lock);
            g_isr[col].target_abs = g_isr[col].pos_abs;
            g_isr[col].velocity = 0;
            portEXIT_CRITICAL(&g_lock);
            if (a.state.load(RLX) == AxisState::Moving) a.state.store(AxisState::Idle, RLX);
            break;

        case ReqKind::None:
            break;
    }
    (void)pos;
}

void control_tick() {
    // One consistent snapshot of the tunables for the whole tick.
    MotionParams p;
    portENTER_CRITICAL(&g_lock);
    p = g_params;
    portEXIT_CRITICAL(&g_lock);

    for (int i = 0; i < N_COLUMNS; ++i) {
        Axis& a = g_ax[i];

        // Single critical section: take the ISR snapshot and drain the mailbox
        // together, so a command can never be applied against a stale position.
        int64_t pos, hall, tgt;
        uint32_t seq;
        bool hall_now;
        Request req;
        portENTER_CRITICAL(&g_lock);
        pos = g_isr[i].pos_abs;
        hall = g_isr[i].hall_abs;
        tgt = g_isr[i].target_abs;
        seq = g_isr[i].hall_seq;
        hall_now = g_isr[i].hall_active;
        req = a.req;
        a.req.kind = ReqKind::None;
        portEXIT_CRITICAL(&g_lock);

        if (req.kind != ReqKind::None) {
            apply_request(i, a, req, pos, p);
        }

        const bool edge_arrived = (seq != a.seq_seen);
        if (edge_arrived) {
            a.seq_seen = seq;
            on_hall_edge(i, a, hall, pos, p);
        }

        // Either of the above may have moved the target.  Note this must test
        // the latched flag, not seq != a.seq_seen, which on_hall_edge just
        // cleared by assigning seq_seen.
        if (req.kind != ReqKind::None || edge_arrived) {
            portENTER_CRITICAL(&g_lock);
            tgt = g_isr[i].target_abs;
            portEXIT_CRITICAL(&g_lock);
        }

        switch (a.state.load(RLX)) {
            case AxisState::Unhomed:
                if (a.home_delay > 0 && --a.home_delay == 0) begin_home(i, a, pos, hall_now, p);
                break;

            case AxisState::Homing:
                if (a.home_phase == HomePhase::Release) {
                    if (!hall_now) {
                        a.home_phase = HomePhase::Seek;
                        set_target_locked(i, pos + HOME_LIMIT);
                    } else if (pos >= tgt) {
                        enter_fault(i, a, "hall never released");
                    }
                } else if (a.home_phase == HomePhase::Seek) {
                    if (pos >= tgt) enter_fault(i, a, "no hall edge in 1.2 revolutions");
                } else if (a.home_phase == HomePhase::Settle) {
                    if (pos >= tgt) {
                        a.home_phase = HomePhase::None;
                        a.index.store(RING_HOME_SLOT, RLX);
                        a.rehome_retries = 0;
                        a.state.store(AxisState::Idle, RLX);
                        ESP_LOGI(TAG, "col %d homed at pos=%lld", i, static_cast<long long>(pos));
                    }
                }
                break;

            case AxisState::Moving:
                if (pos >= tgt) {
                    // RING_INVALID after open-loop stepping: the drum is still
                    // position-tracked, but what is on the front is unknown.
                    a.index.store(a.dest_index.load(RLX), RLX);
                    a.state.store(AxisState::Idle, RLX);
                } else if (a.hall_valid.load(RLX) && edge_overdue(pos, hall)) {
                    enter_fault(i, a, "missed hall edge while moving");
                }
                break;

            case AxisState::Idle:
            case AxisState::Fault:
                break;
        }

        const AxisState now = a.state.load(RLX);
        const bool running = (now == AxisState::Moving || now == AxisState::Homing);
        if (!running) {
            set_velocity_locked(i, 0);
        } else {
            int32_t v;
            portENTER_CRITICAL(&g_lock);
            v = g_isr[i].velocity;
            portEXIT_CRITICAL(&g_lock);
            const RampParams rp{a.v_max, p.accel, CONTROL_HZ};
            set_velocity_locked(i, ramp_next_velocity(tgt - pos, v, rp));
        }
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

bool valid_col(int c) { return c >= 0 && c < N_COLUMNS; }

void post(int col, const Request& r) {
    portENTER_CRITICAL(&g_lock);
    g_ax[col].req = r;
    portEXIT_CRITICAL(&g_lock);
}

// Index 0 only has a position modulo one revolution.  Keeping the offset in
// [0, rev) keeps it forward of the Hall edge, which the settle phase relies on.
int32_t normalize_cal(int32_t usteps) {
    const int32_t rev = static_cast<int32_t>(USTEPS_PER_SPOOL_REV_NOMINAL);
    return ((usteps % rev) + rev) % rev;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API.  Commands are posted to the control task; they do not mutate the
// FSM here.  Return values validate arguments and give the CLI immediate
// feedback; the control task re-validates authoritatively when it drains.
// ---------------------------------------------------------------------------

esp_err_t init(const MotionParams& p) {
    g_params = p;

    for (int i = 0; i < N_COLUMNS; ++i) {
        g_step_bit[i] = pin_mask(PIN_STEP[i]);
        g_hall_bit[i] = pin_mask(PIN_HALL[i]);
        g_isr[i] = AxisIsr{};
        g_ax[i].cal_offset.store(normalize_cal(p.cal[i]), RLX);
    }
    g_hall_invert = p.hall_active_low ? HALL_MASK_ALL : 0u;

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
        g_ax[i].cal_offset.store(normalize_cal(p.cal[i]), RLX);
    }
}

MotionParams params() {
    MotionParams p;
    portENTER_CRITICAL(&g_lock);
    p = g_params;
    portEXIT_CRITICAL(&g_lock);
    for (int i = 0; i < N_COLUMNS; ++i) p.cal[i] = g_ax[i].cal_offset.load(RLX);
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
            r.delay_ticks = 1 + i * HOME_STAGGER_MS;  // control ticks == ms
            post(i, r);
        }
        return ESP_OK;
    }
    if (!valid_col(col)) return ESP_ERR_INVALID_ARG;
    r.delay_ticks = 1;
    post(col, r);
    return ESP_OK;
}

esp_err_t go(int col, int index) {
    if (!valid_col(col) || !ring_index_valid(index)) return ESP_ERR_INVALID_ARG;

    const Axis& a = g_ax[col];
    const AxisState s = a.state.load(RLX);
    if (s != AxisState::Idle && s != AxisState::Moving) return ESP_ERR_INVALID_STATE;
    if (!a.hall_valid.load(RLX)) return ESP_ERR_INVALID_STATE;  // no home reference

    Request r;
    r.kind = ReqKind::Go;
    r.index = index;
    post(col, r);
    return ESP_OK;
}

esp_err_t step_open_loop(int col, int64_t usteps, int32_t flaps_s) {
    if (!valid_col(col) || usteps < 0 || flaps_s <= 0) return ESP_ERR_INVALID_ARG;
    if (g_ax[col].state.load(RLX) == AxisState::Homing) return ESP_ERR_INVALID_STATE;

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
    g_ax[col].cal_offset.store(norm, RLX);
    portENTER_CRITICAL(&g_lock);
    g_params.cal[col] = norm;
    portEXIT_CRITICAL(&g_lock);
    return ESP_OK;
}

esp_err_t adjust_cal(int col, int32_t delta) {
    if (!valid_col(col)) return ESP_ERR_INVALID_ARG;
    return set_cal(col, g_ax[col].cal_offset.load(RLX) + delta);
}

void info(int col, AxisInfo& out) {
    if (!valid_col(col)) return;
    const Axis& a = g_ax[col];

    // Position, target and velocity must agree with each other, so they come
    // from the spinlock group rather than from separate atomics.
    portENTER_CRITICAL(&g_lock);
    out.pos_abs = g_isr[col].pos_abs;
    out.hall_abs = g_isr[col].hall_abs;
    out.target_abs = g_isr[col].target_abs;
    out.velocity = g_isr[col].velocity;
    out.hall_level = g_isr[col].hall_active;
    portEXIT_CRITICAL(&g_lock);

    out.state = a.state.load(RLX);
    out.index = a.index.load(RLX);
    out.dest_index = a.dest_index.load(RLX);
    out.cal_offset = a.cal_offset.load(RLX);
    out.hall_valid = a.hall_valid.load(RLX);
    out.revs = a.revs.load(RLX);
    out.resync_minor = a.resync_minor.load(RLX);
    out.resync_major = a.resync_major.load(RLX);
    out.faults = a.faults.load(RLX);
    out.last_hall_err = a.last_hall_err.load(RLX);
    out.hall_to_hall = a.hall_to_hall.load(RLX);
    // Derived from the step count rather than counted per move, so it stays
    // exact across wraps and open-loop stepping.
    out.flips_total =
        static_cast<uint32_t>((out.pos_abs * USTEPS_PER_FLAP_DEN) / USTEPS_PER_FLAP_NUM);
}

bool all_idle() {
    for (int i = 0; i < N_COLUMNS; ++i) {
        const AxisState s = g_ax[i].state.load(RLX);
        if (s == AxisState::Moving || s == AxisState::Homing) return false;
    }
    return true;
}

}  // namespace motion

const char* axis_state_name(AxisState s) {
    switch (s) {
        case AxisState::Unhomed: return "UNHOMED";
        case AxisState::Homing:  return "HOMING";
        case AxisState::Idle:    return "IDLE";
        case AxisState::Moving:  return "MOVING";
        case AxisState::Fault:   return "FAULT";
    }
    return "?";
}

}  // namespace swan
