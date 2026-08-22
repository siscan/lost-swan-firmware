# Motion synchronisation contract

One page. If a change to `components/motion/` contradicts this file, one of the
two is wrong — fix whichever it is, in the same commit.

The code splits into a **pure core** (`axis_control.h/.cpp` — FSM, homing, edge
verification, mailbox semantics; no IDF includes, driven directly by the
host-side simulated-axis suite) and an **IDF shell** (`motion.cpp` — GPIO,
GPTimer ISR, FreeRTOS task, spinlock, logging). The core cannot take locks by
construction: it consumes a snapshot and returns the writes to apply. All
synchronisation lives in the shell.

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

**(3) control tick → tasks — relaxed atomics.** Everything published
(`state`, `index`, counters…) is a single value that is independently
meaningful; none of them publishes ownership of other memory, so
`memory_order_relaxed` is sufficient and no fence is needed. Anything that
needs a *consistent multi-field view* (position vs target vs velocity) must go
through the spinlock instead — that is why `info()` takes it. A
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
