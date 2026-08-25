# Planned work, not built

Design notes for features that are **decided in shape but deliberately not
implemented**. Nothing here is in the firmware. The point of the file is that a
later session finds the shape already agreed, and that today's code does not
quietly make it impossible.

Each entry says what it is, what in the shipped firmware it depends on, and
what would have to be built. Anything that turns out to conflict with a shipped
contract is called out here rather than discovered halfway through.

---

## The scriptable zero choreography ("show creator")

**Status: planned, post-hardware. Do not build it before the mechanism is
characterised** — every interesting thing a script can ask for (a slow
throw, a stagger, a per-column speed) is a statement about drums nobody has
turned yet, and the numbers it would be written against are the ones
`docs/BRINGUP.md` steps 3–7 exist to settle.

### The shape

The zero → reveal sequence becomes an **interpreted script** rather than three
config values. JSON in LittleFS, uploadable exactly like a ring table, with a
browser editor later. It drives the motion primitives that already exist plus
the few that do not (below): per-column speeds, spins, staggered landings,
deliberate slow-throughs.

**The current behaviour is the built-in default script**: hold `000:00` for
`countdown.zero_hold_s` (3 s), spin all five open-loop at
`motion.flaps_s_alarm` for `countdown.spin_s` (6 s), land on
`countdown.reveal[5]`. A display with no uploaded script behaves exactly as it
does today, and that must stay true — the default is not a fallback for a
broken upload, it is the show.

### Contracts to protect, checked against what shipped (2026-08-25)

Four things were checked. Three are already compatible; one is a genuine gap.

1. **`cfg.zero_hold_s` and `cfg.spin_s` must keep existing and keep meaning
   "when does the reveal land".** The terminal prop reads both off retained
   `swan/state` and schedules its own SYSTEM FAILURE beat against them; it
   holds no duplicate of 3 and 6 (spec §7.3, §12). Pinned by
   `test_api.cpp`'s `test_state_document_contract`, which also asserts they sit
   inside the MQTT change window.

   **Compatible.** They are emitted at one call site from a config snapshot
   (`.kv("zero_hold_s", cfg.zero_hold_s)` in `components/webapi/api.cpp`), so a
   script makes them *derived* rather than configured without moving, renaming
   or retyping anything. **A script MUST publish equivalents** — either those
   two keys computed from the script, or a total `reveal_delay_s` **in addition
   to** them, never instead of. Removing either is a change in another
   repository.

   The honest difficulty, recorded now: a script whose reveal timing is not
   knowable in advance (a stagger whose length depends on where the spin
   stopped) cannot publish a truthful `spin_s`. If that case arises, publish
   the script's *nominal* total and rely on the `reveal` announcement for the
   real one — which is what the announcement exists for.

2. **The `reveal` event stays the landing beat, whatever the script did.**

   **Already true, and needs no work.** The announcement fires on
   `sched_.desired() == reveal_frame() && sched_.settled()` — the columns
   actually arriving at the reveal frame, regardless of how they got there
   (`mode_manager.cpp`). A script that spins, staggers and lands trips exactly
   the same condition. This is also why the beat is *announced* rather than
   estimated (spec §7.3): under a script, estimating it would be hopeless.

3. **The maintenance hold applies to any script.**

   **Already true.** `const bool held = maintenance_ || ota_hold_ || no_drivers`
   gates the modes tick before any countdown machinery runs, and a deadline
   that passes while held wakes silently into the reveal rather than replaying.
   A script interpreted from the same tick inherits all of it. Do not give the
   interpreter its own task — that is how it would escape the hold.

4. **Per-column speed and staggered landing DO NOT EXIST as primitives.** This
   is the gap.

   - `FrameScheduler` carries **one** `flaps_s` for all five columns
     (`frame.h`, from `motion.flaps_s_normal`). Only `spin(i, flaps_s, seconds)`
     is per-column, and it is open loop — it ends with the index unknown.
   - `land_on_tick` deliberately **synchronises**: it starts the frame early by
     the *longest* column's modelled duration so every column lands together.
     A deliberate stagger is the opposite of what it does, and needs per-column
     lead offsets rather than one.
   - A **slow through** — "go to slot 12 the long way, twice round" — is not
     expressible at all. Every move is `(target − current) mod 50`, so extra
     revolutions cannot be requested. Today the only way to turn a drum more
     than once is `spin`, which surrenders the index.

   So the script interpreter is not purely a scheduling layer over existing
   primitives: it needs a per-column speed on ordinary moves, a per-column
   lead offset, and a closed-loop multi-revolution move. All three are motion
   changes and must keep `docs/MOTION_SYNC.md` true.

### Also worth knowing before starting

- **Upload path**: follow the ring's, which is the pattern that survived
  review — validate entirely into a staging object on the HTTP task, let the
  **modes task** apply it, then temp-write-and-rename. `ring_store.h` states
  the contract. A rejected upload must leave the running script untouched and
  never reach the filesystem.
- **Parsing**: the ring upload streams (`ring/json_stream.h`) because a
  `json::Value` DOM is ~64 bytes per node against a 2-byte token and this board
  has no PSRAM and no exceptions — a failed allocation is `abort()`. A script
  document is small and random-access, so the DOM is probably right, but it
  must carry the untrusted caps (`MAX_NODES_UNTRUSTED = 700`, 256 elements per
  container) and the heap guard.
- **One dispatcher.** A script command is a `§10.2a` command like everything
  else, reachable from MQTT by construction. Do not add a network path.
- **`countdown.reveal[5]` stays the landing frame** and stays resolved by
  **name** against whichever ring loaded (spec §11) — a script must not write
  ring indices, for the same reason nothing else may: the same index is a
  different character on column 5.
- The wear cost of a script is computable the way everything else is —
  `components/modes/wear.cpp` walks real renderers against the loaded ring. A
  script that flips a drum ten thousand times a run should say so in the
  editor rather than being discovered on the cards.
