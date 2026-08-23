# FIRMWARE HANDOFF — LOST Swan split-flap

Everything firmware needs that isn't in MECHANICAL_README.md or BOM.md.
Facts below are either **DECIDED** (locked by design/measurement) or
**UNCERTAIN** (not yet chosen, ordered, or physically verified — do not
hard-code; make it a config value).

## 1. Drive geometry — DECIDED
- Drum gear: teeth cut into the spool disc rim, **Z = 85**, module **1.5**,
  involute. Disc OD over teeth 130.5 mm.
- Pinion on motor shaft: **Z = 33**, module 1.5. Centre distance 88.5 mm.
- Ratio motor:drum = **85 / 33** (≈ 2.5758 : 1).
- Flaps per drum: **50**.
- Motor: 1.8° (200 full steps/rev), driven at **1/16 microstep** →
  3200 µsteps/motor-rev.
- **µsteps per flap = 3200 × 85 / (33 × 50) = 5440/33 ≈ 164.8485** — NOT an
  integer. A per-column **phase accumulator** (integer numerator 5440,
  denominator 33, carried across moves) is mandatory; naive rounding per
  flip accumulates ~5 flaps of error per hour of continuous stepping.
- µsteps per full drum revolution = 272000/33 exactly.

## 2. Ring semantics — **SUPERSEDED 2026-08-22 by FIRMWARE_SPEC.md §4**

> This section described the **v2 ascending single ring**.  The ring was
> redesigned to **v3 descending, two rings** (manifests `manifest_cols1234.json`
> and `manifest_col5.json`), and `manifest.json` no longer exists.  The spec
> §4 is authoritative; this section is corrected below so it cannot mislead,
> but do not treat it as the source of truth.

What changed, and why:

- **Descending, not ascending.**  One forward flip now *decrements* the
  displayed digit, so a countdown tick is 1 flip on every column.  Clock
  increments became the expensive direction (49 flips on ring A, 24 on ring B),
  managed by `clock.granularity_min` (spec §7.1).
- **Two rings, three part numbers.**  Ring A on columns 1–4; ring B on column
  5.  Ring order is identical for cols 1–4, but card colour is not, so cols 1–3
  and col 4 are separate parts: cols 1–3 (black cards, 0 straddles), col 4
  (white/red cards, straddles at flaps 1 and 37), col 5 (own ring, straddles at
  0, 14, 24, 39).
- **Digit placement.**  Ring A: digits at slots 40–49, `slot = 49 − digit`.
  Ring B: **two** blocks, slots 15–24 (`slot = 24 − digit`) and 40–49
  (`slot = 49 − digit`), 25 apart, so column 5's 0→9 wrap costs 16 flips
  instead of 41.  A digit therefore does not have one slot on column 5.
- **Blank = home = slot 0** on both rings.  Still true.
- **AM/PM:** ring A slots 38 (PM) and 39 (AM).  **Absent from ring B** — AM/PM
  renders on column 1, not column 5.  The claim below that "column 5 carries
  AM/PM" was already contradicted in its own paragraph and is now impossible;
  spec §7.1 puts AM/PM on column 1 and explains the geometry.
- **WiFi glyph:** ring A slot 1.  **Absent from ring B** — it renders on the
  centre column (column 3), which is ring A.
- **`?`:** ring A slot 17, ring B slot 25.  The `?????` state is every column
  to its own question slot; both rings carry one.
- **Countdown-zero display:** still user-chosen, still a config array
  (`countdown.reveal[5]`) — but note an index means a different character on
  column 5, so pick these by name once the web UI exists.
- **Countdown layout:** MMM:SS with live seconds is now the default
  (`countdown.seconds_mode = seconds`), which the two-block column 5 made
  affordable.  MMM:S0 survives as the low-wear `tens` option.

## 3. Homing — DECIDED in principle, geometry partly UNCERTAIN
- One **Ø6×3 N35 magnet** per drum, glued into the pocket in the **idler-side
  disc** at **R52** (pocket Ø6.2 × 3.0 deep, faces outward past the idler
  disc's outer face).
- One digital Hall switch (BOM: **A3144, unipolar**) per column, mounted on
  the **idler side**, fixed to the module wall, gap ~1-2 mm from the disc
  face as it passes R52.
- Homing = seek Hall edge, then apply a **per-column software calibration
  offset** (µsteps, stored in NVS) mapping the magnet edge to slot-0
  centred. The magnet's angular position relative to slot 0's pockets is an
  **assembly convention**, not a print feature — hence the offset.
- **UNCERTAIN:** exact sensor bracket position/geometry (not yet designed);
  which Hall edge (approach vs. leave) is cleaner; magnet polarity facing
  the sensor (A3144 is unipolar — BOM says bench-test one pair and mark the
  working face before gluing all ten).

## 4. Rotation direction — UNCERTAIN
Drums must rotate so flap fronts fall forward (ascending slots). The
physical CW/CCW as seen from the motor depends on which side the motor
mounts and pinion meshing — **not yet fixed**. Provide a per-column
`invert_dir` boolean; assume all five identical once one is verified.

## 5. Electronics — DECIDED
- Controller: **ESP32-C5** (in hand). Build on current ESP-IDF / latest
  Arduino core — the C5 is new; old cores lack it.
- Drivers: **TMC2209 ×5, standalone mode** (no UART — TMC addressing tops
  out at 4/bus and we have 5). MS1 = MS2 = high → 1/16. Internal ×256
  interpolation stays on (default). Standstill current auto-reduction stays
  on (default) — it is the holding-torque plan.
- EN lines ganged to one GPIO (active low). STEP + DIR per column.
- GPIO budget: 5×STEP + 5×DIR + 1×EN + 5×HALL + 3×I2S + LED ≈ 20 pins.
  **Pin map not yet assigned** — firmware's choice.
- Audio: **MAX98357A I2S** mono amp + 40 mm 4 Ω speaker. WAVs in
  LittleFS. Gain pin default (9 dB). Volume + mute in the web UI.
- Power: 12 V 6 A brick → drivers; buck to 5 V for logic/sensors/audio.
- Hall supply: **A3144 requires 4.5-24 V — run it at 5 V.** Output is
  open-collector: pull-up to **3V3** (10 k) at the ESP32 pin so the input
  never sees 5 V. (Recommended-not-yet-wired; if sensors are swapped for a
  3.3 V-capable part later, same firmware.)
- Network: WiFi (C5 is dual-band), NTP for clock, web UI + MQTT for Home
  Assistant, OTA.

## 6. Electronics — UNCERTAIN (do not hard-code)
- **Motor model: not yet ordered.** BOM recommends 17HS4401-class
  (200 steps, 1.5-1.7 A, 5 mm D-shaft). Firmware only cares about
  200 steps/rev; keep steps/rev a constant anyway.
- **TMC2209 vendor: not chosen** (BTT vs FYSETC). Vref formula is
  vendor-specific; target ≈ 1.1-1.2 A RMS. Hardware-side concern only.
- **JST pin order: TBD.** Plan is JST-XH: 4-pin motor (coil pairs), 3-pin
  Hall (5V / GND / OUT), MorganManly-style chaining — exact pin assignment
  to be fixed when the harness is built. Document it in this file's repo
  copy when crimped.
- Whether Hall OUT gets its pull-up at the sensor or at the board: TBD.

## 7. Behavioural spec highlights (from the design log)
- Modes: **clock** (default, ~99 % of life), **message board** (5 glyphs;
  no alphabet exists on the ring), **108-minute countdown** with
  hieroglyph reveal at zero, plus the boot/WiFi-seeking state.
- Audio cues: Swan 4-minute warning beeps, escalating alarm, system-failure
  cacophony at zero. Flap motion itself is the ambient sound; audio only
  for countdown states and alerts.
- Max flip rate target ≈ 25 flaps/s burst (alarm spin) ≈ 4.2 k µsteps/s
  per motor; all five simultaneous ≈ 21 k steps/s total — timer-ISR
  bit-bang territory, no PIO needed.
- Egyptian idle flourishes (per-ankh at boot, Ra at noon, etc.) were
  discussed as optional flavour — config-gated, not core.

## 8. Open mechanical investigation (context, not firmware-blocking)
As of packaging: an axial pinch between flap pivot nubs and spool pocket
floors is under caliper investigation; pocket depth will likely change on
the next spool print. Nothing in it affects firmware constants above —
gear counts, ring order, and homing scheme are unchanged by any candidate
fix.

## 9. Files in this package
- `manifest_cols1234.json`, `manifest_col5.json` — the machine-readable v3
  rings (descending; ring A for cols 1-4, ring B for col 5).  These replaced
  the single v2 `manifest.json` on 2026-08-22.
- `RING_ORDER.md` — the v2 ascending order, human-readable.  **Historical**:
  it documents the retired ring.
- `MECHANICAL_README.md`, `BOM.md` — verbatim copies.
- `ring_poster.png` — the 50-character visual reference.
