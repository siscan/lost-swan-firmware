# Motion synchronisation contract

One page. If a change to `components/motion/` contradicts this file, one of the
two is wrong — fix whichever it is, in the same commit.

The code splits into a **pure core** (`axis_control.h/.cpp` — FSM, homing, edge
verification, mailbox semantics; no IDF includes, driven directly by the
host-side simulated-axis suite) and an **IDF shell** (`motion.cpp` — GPIO,
GPTimer ISR, FreeRTOS task, spinlock, logging). The core cannot take locks by
construction: it consumes a snapshot and returns the writes to apply. All
synchronisation lives in the shell.

## The tasks, and their priorities

A newcomer picking a priority for a new task has to know these, and they existed
only as comments in six different files.  Single core, so a higher number
genuinely preempts.

| task | priority | stack | watched? | why there |
|---|---|---|---|---|
| WiFi (IDF) | 23 | — | no | its own hard deadlines |
| `swan_motion` — the 1 kHz control tick | **19** | 4096 | **yes** | above lwIP (18) so networking can never delay a control tick |
| lwIP (IDF) | 18 | — | no | |
| `swan_modes` — the 20 Hz mode tick | **5** | 8192 | **yes** | above the transports, below motion |
| `swan_audio`, `swan_mqtt`, esp-mqtt, `swan_reboot` | 4 | 4096–8192 | no | all block on purpose |
| httpd, `swan_dns` | 3 | 8192 / 3072 | no | httpd blocks on recv by design |
| `swan_soak`, `swan_status`, console REPL | 2 | 4096 / 3072 / 4096 | no | drivers and cosmetics, not deadlines |
| `swan_journal`, `swan_otachk` | 1 | 4096 / 3072 | no | a journal write is never the most important thing happening |
| IDLE0 | 0 | 1536 | **yes** | the only coverage for "something above 0 is spinning" |

**A new task belongs at 1–4 unless you can say why not.**  Anything at 6 or
above silently preempts the modes task; anything at 20 or above delays the
control tick.  Both watched tasks must feed the watchdog on every loop, and the
timeout is 30 s.

## Lock ordering, in one line

`ModeManager::mu_` is the innermost lock that matters: the cue sink and the
journal sink are both called **while it is held**, so nothing reachable from
either may take `Context::dispatch_mu`, re-enter ModeManager, or block.  That
rule has been broken twice (the cue sink deadlocked the modes task in Phase 5;
the journal sink was written to it from the start because of that).

## Who owns what

| data | owner (sole writer) | read by | mechanism |
|---|---|---|---|
| `AxisIsr` per axis: `pos_abs`, `hall_abs`, `hall_seq`, `hall_hist`, `hall_active`, `accum` | **step ISR** | control tick, `info()` | spinlock `g_lock` |
| `AxisIsr.target_abs`, `AxisIsr.velocity` | **control tick** (via write-back) | step ISR | spinlock `g_lock` |
| `g_hall_invert`, `g_step_bit[]`, `g_hall_bit[]` | init / `set_params` | step ISR | spinlock (invert); bits are set once before the timer starts |
| `Request g_req[]` mailbox | **any task posts**, control tick drains | control tick | spinlock `g_lock` |
| `AxisCtl` atomics: `state`, `index`, `dest_index`, `hall_valid`, `revs`, `resync_minor/major`, `faults`, `last_hall_err`, `hall_to_hall` | **control tick** | any task | `std::atomic`, relaxed |
| `AxisCtl.cal_offset` | **calibration API** (any task) | control tick | `std::atomic`, relaxed |
| `AxisCtl` plain fields: `home_phase`, `v_max`, `hall_prev`, `seq_seen`, `home_delay`, `rehome_retries` | **control tick** | nobody else | none needed — private |
| `g_params` | `set_params` / `set_cal` | control tick (snapshot per tick), `params()` | spinlock `g_lock` |
| `g_isr_ticks` | **step ISR** | control tick | `volatile`, single writer, wrap-safe by construction (see below) |
| `g_park_pending[]` | `set_columns` (any task) sets, control tick clears | control tick, `republish_masks` | plain `bool`; see below |

**`g_isr_ticks`** is the step ISR's liveness counter, added in Phase 6 because
the task watchdog covers the 1 kHz control task and *nothing covered the thing
that actually moves the drums*: if the GPTimer stops, the control task keeps
looping and feeding the watchdog while the display stands still. It is a plain
`volatile uint32_t` rather than an atomic on purpose — one writer (the ISR),
one reader (the control tick), and the reader only ever asks "is this different
from last time", which a torn read cannot get wrong in a way that matters: a
stale value costs one extra tick of patience against a 200-tick threshold, and
wrap-around is a change like any other.

**`g_park_pending[]`** is how a column being disabled parks itself on blank
before its drive bit goes away. `set_columns` may run on any task and only sets
the flag; the control tick issues the move, watches for the column to reach the
home slot, clears the flag and re-publishes the masks. The flag is deliberately
NOT under the spinlock: it is written once by one task, read by one task, and a
one-tick-late observation is harmless — whereas posting the park directly from
`set_columns` raced `republish_masks()` and lost, which is the bug it exists to
fix (the axis reported itself parked and the drum never moved).

## The three boundaries

**(1) step ISR ↔ tasks — spinlock.** The ISR cannot take a lock, so the task
side takes `portMUX g_lock`, which disables interrupts for its duration and
therefore excludes the ISR. Critical sections are short and fixed in number
per control tick: one to snapshot `g_params` for the whole tick, then per axis
one to snapshot the ISR state *and* drain its mailbox, and one to write back
`target`/`velocity` - three kinds in total. `info()` adds one more so that
position, target and velocity are mutually consistent. Nothing loops,
allocates, or logs inside a critical section.

**(2) tasks → control tick — single-slot mailbox.** One `Request` per axis,
posted under the lock, drained under the same lock *together with* the
snapshot, so a command is never applied against a position newer than the one
it will be validated with. Replace-on-write: a newer command posted before the
drain supersedes the older one whole — exactly spec §6's "a new frame while
moving simply replaces targets". A superseded command is never partially
applied, and a stale command is never applied after a newer one (asserted by
the simulated-axis suite). The control tick is the **sole writer of the FSM**,
so command validation there is authoritative; the checks in the public API are
advisory, for immediate CLI feedback.

**(3) control tick → tasks — relaxed atomics under a seqlock.** Every
published field (`state`, `index`, counters…) is stored relaxed and is
independently meaningful on its own. **No consumer may read two or more of
them and treat them as a consistent pair** — relaxed stores to different
atomics may be observed in either order. Readers that need a pair (`state` +
`index` for "settled at index i"; `revs` + `hall_to_hall` for "the length of
revolution N"; `state` + `hall_valid` for the `go` pre-check) use
`axis_read_published()`, which brackets the loads with `AxisCtl::seq`: the
control tick increments `seq` (acq_rel) on entry to `axis_control_tick` and
again (release) on exit, so a reader that sees the same even `seq` before and
after its loads has a view from one tick. Retry is rare — the writer holds
the odd state for microseconds, once a millisecond. `info()`, the `go`
pre-check and the Phase 2 frame scheduler all go through it; `cal_offset` is
written by the calibration API outside the bracket and is a single value.
Anything needing ISR-side consistency (position vs target vs velocity) still
goes through the spinlock — that is why `info()` takes both. A
`static_assert` requires all these atomics to be lock-free: a libatomic
fallback would take a lock the ISR could interrupt.

## Placement rules (IRAM/DRAM)

- The ISR is `IRAM_ATTR`; everything it touches (`g_isr[]`, the bit masks,
  `g_hall_invert`) is `DRAM_ATTR`. NVS and OTA writes disable the flash cache;
  a flash-resident ISR or data access stalls and drops steps (spec §5.2).
- The per-axis ISR work is the core's `axis_isr_dda` / `axis_isr_hall`,
  declared `always_inline` so they fold into the caller and inherit its IRAM
  placement — the ISR must never call flash-resident code. `dda_tick`, which
  `axis_isr_dda` calls, carries the same attribute for the same reason. The
  host's fake ISR calls the same two functions, which is what makes the
  simulation faithful.
- `AxisIsr` is a separate struct from `AxisCtl` on purpose: the DRAM boundary
  is visible in the type system, not asserted in a comment.

## Ordering facts the correctness argument rests on

- The Hall edge latches `hall_abs = pos_abs` **inside the ISR**, so edge
  position and step count can never disagree.
- Within one control tick the core processes: request → Hall edge → FSM →
  velocity. `enter_fault` may schedule a re-home that `begin_home` starts in
  the same tick.
- The write-back applies at most one target and exactly one velocity per axis
  per tick; the ISR emits at most one step per 20 µs tick (velocity is clamped
  to `TICK_HZ`), which is what permits a single GPIO bank write for all five
  axes.

## Honest scope of the host-side verification

The simulated-axis suite executes the real core (drain-then-apply ordering,
FSM validation, whole-request application) but replicates the single-slot
replace itself in the harness — the shell's `post()`/drain under the spinlock
cannot be host-compiled. The lock-protected assignment is one statement;
what the lock guarantees (drain and snapshot in the same critical section) is
a shell property, reviewed by reading, not tested by running.

One deliberate behaviour change from the pre-split shell: `enter_fault` no
longer forces velocity to zero before the automatic re-home. The ramp
converges onto the homing speed instead — a smooth forward transition rather
than a step discontinuity, safe because rotation is forward-only.

## The mailbox lag, and why the scheduler remembers what it posted

Added after the Phase 3 adversarial review.

A command posted to an axis is **not** visible in what that axis reports until
the 1 kHz control tick drains the single-slot mailbox — up to a millisecond
later, and always after the 20 Hz modes tick that posted it has finished.  So
within one modes tick, `MotionPort::col()` describes the state *before*
anything that tick commanded.

`FrameScheduler` used to reason purely from `col()`, and that is wrong twice:

- **`issue()` skipped a column** whose new target happened to equal its stale
  reported index, leaving a superseded command in the mailbox to execute
  instead.  `enter_mode()` runs a convergence pass before the caller issues its
  own frame, so `preset.set blank` right after an automatic re-home sent four
  columns blank and the fifth 44 slots the other way.
- **The convergence pass overwrote a spin** it had just started, because those
  columns still reported `Idle` at their old index.  With `zero_hold_s = 0`
  that happened every run.

`FrameScheduler::posted_` closes both: per column, what was commanded during
the current tick (`kNotPosted`, an index, or `RING_INVALID` for a spin).
`issue()` skips only an exact repeat; the convergence pass skips any column
already commanded this tick.  It is stamped with the tick's `now_ms` and
cleared by whichever of `show()` / `spin_all()` / `tick()` first sees a new
one — so a frame issued *after* the convergence pass in the same tick still
sees what that pass commanded, which is the whole point.

This is bookkeeping inside one task, not a synchronisation primitive: the
scheduler runs only on the modes task.  `test/host/fake_port.h` grows a
`mailbox_lag` mode that models the drain, because the instant-settling fake
could not express the bug.

