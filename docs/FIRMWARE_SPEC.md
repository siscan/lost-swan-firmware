# LOST Swan split-flap — Firmware Specification

Status: **v1.0 — questions answered 2026-08-21, Phase 1 may start.** Items
tagged `VERIFY` are facts that must be checked against hardware or datasheets
before they are hard-coded. `[Qn]` tags remain where the answer is recorded in
§16; the decision log in §17 is authoritative. Do not silently change either.

Source-of-truth files (ask Nico to drop them into `docs/ref/`): `BUILD/README.md`
(v6 mechanical spec + decision log), `BUILD/BOM.md`, and the ring
`manifest.json` from the flap pipeline. Constants in this spec were derived from
those; the files win on conflict.

---

## 1. What the device is

A wall-mounted five-column split-flap display reproducing the Swan station
countdown timer. Runs as a **clock ~99% of the time**, a **five-glyph message
board** occasionally, and a **108-minute countdown** on demand. At zero the
columns spin and land on hieroglyphs, with the Swan alarm sounds.

Columns are physically grouped 3 + 2 with a band between (no colon column).

---

## 2. Hardware (as specified in the BOM)

| Item | Spec | Notes |
|---|---|---|
| MCU | **ESP32-C5-DevKitC-1-N8R8** — ordered 2026-08-21 (Amazon, third-party seller). Pin map in §2.2. | 8 MB flash, 8 MB PSRAM, USB-Serial-JTAG + USB-UART bridge. **Chip revision unverified until arrival**: the bootloader log prints `chip revision: v1.0` on first `idf.py flash monitor`; anything reporting v0.1 goes back. XIAO ESP32-C5 (also ordered) becomes the spare / project-#2 board; Pico 2 W remains plan B. |
| Motors | 5 × NEMA 17, 17HS4401-class, 1.8° (200 full steps/rev) | `VERIFY` step angle on the motors actually bought. |
| Drivers | 5 × TMC2209 modules, **standalone (no UART)** | MS1 = MS2 = high → 1/16 microstep, internal 256 interpolation. `VERIFY` against the vendor's silkscreen/doc; a different default pull changes every motion constant. Vref set for ~1.1–1.2 A RMS. Standstill current reduction left enabled. |
| Drive | 33T pinion on motor → 85T gear cut into spool disc rim | Ratio **85/33 = 2.5758:1**, not 2.6. `VERIFY` teeth counts against BUILD/README v6. |
| Rotation | **One direction only** (ascending ring). Reverse is mechanically forbidden (flaps jam on the bezel lip). | **DIR is tied to a rail at the drivers** (no GPIO — the XIAO can't spare one; DJ Harrigan's build does the same). Which rail is decided in Phase 1 bench step 3. EN is a single ganged GPIO. |
| Home sensor | 5 × A3144 digital Hall (TO-92), Ø6×3 N35 magnet at R52 on the idler disc, one per column | A3144 supply is 4.5–24 V → must be fed **5 V**; output is open-collector, pull it up to **3V3** (10 k) so the GPIO sees 3.3 V logic. `VERIFY` the hall JST carries 5 V. Output is active-LOW when the magnet is present (`VERIFY`). One operate edge per spool revolution. |
| Audio | MAX98357A I2S mono amp + 40 mm 4 Ω 3 W speaker | 3 GPIOs (BCLK, LRCLK, DIN). Gain pin left at default 9 dB. `VERIFY` SD/shutdown pin handling on the module. No hardware volume → software gain. |
| Power | 12 V 6 A PSU → drivers; 12→5 V buck → logic, halls, amp | All five spinning ≈ 4–5 A on 12 V. |
| Status | Onboard LED on GPIO27 on either C5 board | XIAO: single yellow LED, active low → blink patterns. DevKitC-1: WS2812 RGB → colour-coded status. |
| Button | One user button `[Q6]` | Wired in parallel with the onboard BOOT button (GPIO28, active low) on either C5 board. Costs no GPIO. Must not be held low at reset except to enter the bootloader. |

### 2.0 Board decision — DevKitC-1-N8R8 (ordered; revision check pending)

Three candidates were weighed. The firmware's pin map is one header selected by a
board define, so the decision can be made on arrival; the rest of the spec is
unchanged by it except where §2.4 says otherwise.

ESP32-C5 strapping pins, per Espressif's DevKitC-1 v1.2 user guide:
**GPIO2 (MTMS), GPIO3 (MTDI), GPIO7, GPIO25, GPIO26, GPIO27, GPIO28.** Rule on
any C5 board: only high-impedance loads (the amp's I2S inputs) on strapping
pins. Never a Hall with its pull-up, never EN (driver modules carry pull-ups),
never a shift-register output.

### 2.1 Option A — XIAO ESP32-C5 (ordered)

Signals needed: 5 STEP + 1 EN (ganged) + 5 HALL + 3 I2S = **14 GPIOs**, plus
the button (BOOT pad) and the onboard LED. DIR is tied in hardware.

What the XIAO exposes (Seeed pin map):

| where | XIAO label | GPIO | strapping? | notes |
|---|---|---|---|---|
| header | D0 | 1 | no | LP GPIO, ADC |
| header | D1 | 0 | no | LP GPIO |
| header | D2 | 25 | **yes** | |
| header | D3 | 7 | **yes** | |
| header | D4 / SDA | 23 | no | |
| header | D5 / SCL | 24 | no | |
| header | D6 / TX | 11 | no | UART0 TX — ROM boot log toggles it briefly at reset |
| header | D7 / RX | 12 | no | UART0 RX |
| header | D8 / SCK | 8 | no | |
| header | D9 / MISO | 9 | no | |
| header | D10 / MOSI | 10 | no | |
| back pad | MTDO | 5 | no | JTAG pad |
| back pad | MTCK | 4 | no | JTAG pad |
| back pad | MTDI | 3 | **yes** | JTAG pad |
| back pad | MTMS | 2 | **yes** | JTAG pad |
| back pad | BOOT | 28 | **yes** | onboard button |
| onboard | USER LED | 27 | **yes** | active low |
| onboard | ADC_BAT / ADC_CRL | 6 / 26 | — | battery circuit; not usable |

Non-strapping pins available: 9 on headers + 2 pads = **11**, which is
exactly STEP×5 + EN + HALL×5. I2S goes on strapping pins (fine, high-Z). So
the XIAO needs **four back-pad solder joints** (GPIO5, GPIO4, one of GPIO3/2,
and BOOT) on ~1 mm test pads. Doable, fragile; strain-relieve on the carrier
perfboard. External JTAG is lost, which costs nothing — the C5 debugs over
USB-Serial-JTAG.

Proposed map:

| signal | XIAO pin | GPIO |
|---|---|---|
| STEP1–5 | D6, D7, D8, D9, D10 | 11, 12, 8, 9, 10 |
| EN (ganged, active low) | D0 | 1 |
| HALL1–3 | D1, D4, D5 | 0, 23, 24 |
| HALL4 | MTDO pad | 5 |
| HALL5 | MTCK pad | 4 |
| I2S BCLK / LRCLK | D3, D2 | 7, 25 |
| I2S DIN | MTDI pad | 3 |
| BUTTON | BOOT pad | 28 |
| LED | onboard | 27 |
| spare | MTMS pad | 2 (strapping; high-Z loads only) |
| DIR | — | tied at the drivers |

Power the XIAO from the 5 V buck via its 5V pin. `VERIFY` in Seeed's
schematic whether USB VBUS and the 5V pin are diode-isolated before plugging in
USB while the buck is live.

Antenna: the aluminium faceplate is an RF shield. The u.FL pigtail is an
advantage — route the antenna to the back or an edge of the enclosure. The
bundled antenna is 2.4 GHz only; fine for a wall clock.

**Shift-register variant (rejected):** a 74HC595 for STEP/EN and a 74HC165
for the Halls, bit-banged inside the 50 kHz step ISR, would drop the header
need to ~8 pins and avoid the back pads. Cost: two chips, ~5–10 % of the
single core spent shifting bits, STEP/HALL no longer visible on an MCU pin
when debugging, and the 165's push-pull output must stay off strapping pins.
It is the right tool at 50 modules, not 5. Not planned.

### 2.2 Option B — ESP32-C5-DevKitC-1-N8R8 (v1.2 board, WROOM-1 module)

The board the BOM assumed. Header GPIOs on v1.2: 0, 1, 2, 3, 4, 5, 6, 7, 8,
9, 10, 11, 12, 23, 24, 25, 26, 27, 28 (GPIO15 exists on the header but is
SPICS1 on PSRAM modules; 13/14 are USB D−/D+; 16–22 are flash/PSRAM). That is
**19 broken-out GPIOs**: 12 non-strapping, 5 strapping, plus the RGB LED on
27 and BOOT on 28 — not the "~22" an earlier draft claimed.

Proposed map, everything on headers, nothing on strapping pins except I2S:

| signal | GPIO |
|---|---|
| STEP1–5 | 8, 9, 10, 11, 12 |
| EN (ganged, active low) | 6 |
| HALL1–5 | 0, 1, 4, 5, 23 |
| I2S BCLK / LRCLK / DIN | 7, 25, 26 |
| BUTTON | 28 (onboard BOOT; external button in parallel) |
| LED | 27 (WS2812 RGB — colour-coded status) |
| spare, non-strapping | 24 |
| spare, strapping | 2, 3 (high-Z loads only) |
| DIR | tied at the drivers (a GPIO is affordable here but buys nothing — reverse is forbidden) |

Two USB-C ports (native USB-Serial-JTAG and a USB-to-UART bridge); either
works for the console. PCB antenna on the module (the `U` variant has u.FL).

Purchase gotcha: v1.1 boards exist in the channel and may carry v0.1
engineering-sample chips, which current ESP-IDF no longer supports. Buy from a
source that states v1.2 / chip rev 1.0, and run `esptool chip_id` on arrival.

### 2.3 Option C — Raspberry Pi Pico 2 W (RP2350)

Nico has several coming. Hardware is the best of the three for motion: 26
GPIOs, no strapping-pin dance, twelve PIO state machines (one per axis makes
step generation immune to flash stalls and to WiFi entirely), dual core.
Software is the weakest: 2.4 GHz-only WiFi on the CYW43 companion chip, and
the networking/OTA/provisioning stack would be Arduino-Pico (Earle Philhower
core) plus community libraries rather than ESP-IDF first-party components.
ESP-IDF-specific parts of this spec (§5.2 step ISR, §9 I2S, §10 net, §15
toolchain) would be re-specified; the ring/frame/mode logic and all APIs stay.

Choosing Pico means: PIO step generation, PIO I2S, Arduino-Pico WiFi +
WebServer + WebSockets + MQTT + LittleFS + ArduinoOTA, flash writes guarded by
`__not_in_flash_func` on the motion core. Roughly a third of the spec changes.

### 2.4 Decision

**B (DevKitC-1-N8R8), ordered.** The whole software plan maps onto ESP-IDF first-party
components, the pin map fits on headers with margin, and it is ~$10 and a
few days. **A** works with four back-pad solders and is otherwise identical
firmware. **C** is a legitimate plan B if the C5 purchase is unwelcome; it
costs a re-spec of the net and audio layers, not a rethink. The shift-register
route is the one to avoid.

---

## 3. Derived constants

```
STEPS_PER_MOTOR_REV   = 200                     // VERIFY
MICROSTEPS            = 16                      // VERIFY (MS1=MS2=high)
USTEPS_PER_MOTOR_REV  = 3200
GEAR_DRIVEN_TEETH     = 85                      // VERIFY
GEAR_DRIVE_TEETH      = 33                      // VERIFY
N_RING                = 50
USTEPS_PER_SPOOL_REV  = 3200 * 85 / 33 = 272000/33 = 8242.4242…   (NOT an integer)
USTEPS_PER_FLAP       = 272000 / (33*50) = 5440/33 = 164.8485…     (NOT an integer)
```

Integer target for ring index *i* (0..49) relative to the home edge:

```
T(i) = round(i * 5440 / 33) = (2*i*5440 + 33) / 66      // integer division
```

Speeds (µsteps/s = flaps/s × 164.85):

| flaps/s | µsteps/s | note |
|---|---|---|
| 8 | 1319 | homing / calibration |
| 15 | 2473 | default normal `[default]` |
| 20 | 3297 | |
| 25 | 4121 | default alarm spin `[default]`; ~20.6 k steps/s across five columns |

Flap fall is gravity-limited at roughly 20–25 flaps/s regardless of motor.
The real ceiling is measured in Phase 1, not assumed.

Wrap costs on an ascending ring (cost up + cost down = 50):

| transition | flips | time @ 20 flaps/s |
|---|---|---|
| clock minute-units 9→0 | 41 | 2.05 s (every 10 min) |
| clock minute-tens 5→0 | 45 | 2.25 s (every hour) |
| countdown decrement (any digit −1) | 49 | 2.45 s |
| blank → wifi glyph | 49 | 2.45 s |

---

## 4. Ring (50 positions — a *fluid* data table, not code)

Ascending order. Index 0 is the home/magnet position. The current expected
layout:

| index | character | notes |
|---|---|---|
| 0 | blank | home; shown for leading spaces |
| 1–10 | digits 0–9 | index = digit + 1 |
| 11 | AM | |
| 12 | PM | |
| 13–48 | 36 hieroglyphs | names and order **from `manifest.json`** — do not invent |
| 49 | wifi glyph | boot / no-signal state |

**Nico's answer to Q3:** all drums carry the same characters; columns 4–5 use
a different colour scheme (white card / red card for glyphs, black characters)
versus columns 1–3 (black card, white digits, red glyphs). And the ring order
and placement are **still fluid and may change.** Design consequences:

- The ring table is a **runtime file**, `ring.json` in LittleFS, loaded at
  boot, with a compiled-in copy as fallback. It can be replaced from the web
  UI (Settings → Ring → upload) without reflashing firmware.
- `tools/ringgen.py` generates `ring.json` from the pipeline `manifest.json`.
  Nobody edits `ring.json` by hand.
- Each column may carry its own table (`columns[i].ring`), defaulting to the
  shared one, so a per-column placement change is a file edit.
- **No code references a ring index directly.** The clock renderer asks the
  table for `digit(7)`, `ampm(PM)`, `blank`, `wifi`; the message parser asks by
  name. If a lookup fails, the column shows blank and a diagnostic is raised.
  The simulator renders each column in its own colour scheme from the table.
- Per-column calibration offsets live in NVS, independent of the table, so a
  ring change never invalidates calibration.
- The Calibrate page's index walk shows the *expected* character name beside
  each stop, which is how a table/drum mismatch is caught in seconds.

Rules:
- A move from index *c* to index *t* always costs `(t − c) mod 50` flips.
- Column count and ring size are config, so the second split-flap project
  reuses the codebase.
- Message tokens: ring names from the table, `_` for blank, or `#n` for a raw
  index.
- Roles the code may ask the table for: `blank`, `digit(n)`, `am`, `pm`,
  `wifi`, `question` (the `?` glyph). Anything else is addressed by name.

---

## 5. Motion subsystem

### 5.1 Axis state machine (one per column)

```
UNHOMED → HOMING → IDLE ⇄ MOVING
   any state → FAULT (recoverable via rehome)
```

### 5.2 Step generation

- One GPTimer alarm ISR at `TICK_HZ = 50 000` (20 µs) services all five axes
  with a DDA accumulator per axis. STEP pulses: set high, ≥100 ns, set low
  within the same ISR (TMC2209 tSH ≥ 100 ns), all five via one GPIO bank write.
- The ISR and everything it touches live in IRAM/DRAM
  (`CONFIG_GPTIMER_ISR_IRAM_SAFE`, `IRAM_ATTR`, `DRAM_ATTR`). NVS and OTA flash
  writes disable the cache; a non-IRAM ISR would stall and drop steps.
- Velocity per axis is updated by a 1 kHz control tick (not in the step ISR):
  linear ramp, `ACCEL` default such that 0 → 4121 µsteps/s takes ~50 ms
  (≈ 82 000 µsteps/s²). Short moves are triangular.
- Hall inputs are **sampled in the step ISR** (no separate GPIO interrupt), with
  a 2-of-3 sample filter; the operate edge latches `pos_abs` atomically with the
  step count. This keeps edge position and step position consistent.
- Why not RMT / LEDC / PCNT: peripheral counts on the C5 (`VERIFY` in the TRM —
  believed 2 GPTimers, 2 RMT TX channels, 4 PCNT units) don't cover five axes.

### 5.3 Position model (the non-integer ratio, handled)

Per axis: `pos_abs` (int64, µsteps ever issued, monotonic), `hall_abs` (pos_abs
latched at the most recent operate edge), `cal_offset` (µsteps from the operate
edge to the blank flap sitting correctly; from NVS), `index` (ring index
currently displayed).

- Position of index *i* in the current revolution: `hall_abs + cal_offset + T(i)`.
- Targets are always expressed relative to the **most recent** Hall edge, via
  the **reduced anchor offset** `E(i) = (cal_offset + T(i)) mod one revolution`
  — cal_offset is an arbitrary assembly value in [0, rev), so cal + T(i) can
  exceed a revolution, and the un-reduced form is wrong: a mid-move edge
  rebase would place the target past the *next* edge, which would rebase it
  again, and the move would never terminate (found by adversarial review;
  pinned by test_axis_sim).  A move that has not yet reached `hall_abs + E(i)`
  targets it directly; one that has wraps forward one nominal revolution.
  When an edge arrives mid-move, the target rebases to `hall_abs_new + E(i)`,
  clamped forward (reverse is mechanically forbidden).
- Result: rounding error never exceeds ½ µstep, and the 0.42 µstep/rev residue
  is absorbed at every edge. Nothing accumulates.

### 5.4 Edge verification (every revolution)

At each operate edge: `expected = hall_abs_prev + 8242 (or 8243)`,
`err = actual − expected`.

| `|err|` | action |
|---|---|
| ≤ `HALL_TOL_SILENT` (¼ flap ≈ 41 µsteps `[default]`) | accept, `resync_minor++` |
| ¼ flap < err ≤ 1 flap | accept, `resync_major++`, log warning |
| > 1 flap, or no edge within 8242 + 1 flap of steps | → FAULT routine |

`HALL_TOL_SILENT` must be set after measuring edge repeatability in Phase 1.

FAULT routine: stop the column, attempt re-home up to `REHOME_RETRIES = 3`;
on success resume the current frame; on failure mark FAULT, apply the fault
display policy `[Q5]`, publish via MQTT, set LED pattern, show a UI banner.
`rehome` command clears FAULT.

### 5.5 Homing

1. If the hall is already active at start, step at homing speed until it
   releases (we may be sitting in the magnet zone).
2. Step at homing speed until the operate edge; latch `hall_abs`.
3. Continue to `hall_abs + cal_offset` → `index = 0`, state IDLE.
4. Timeout: 1.2 revolutions without an edge → FAULT.

Boot homes all five columns staggered by `HOME_STAGGER_MS = 250` to limit
inrush. `EN` is asserted only after the drivers have had VM for ≥100 ms.

Note on the wifi glyph: index 49 sits one flap *before* blank, i.e. before the
edge, so it cannot be reached on the homing pass; showing it after boot costs
49 flips (~2.5 s). If the magnet happened to be placed ≥1.5 flaps ahead of
blank this would change, but it is not worth touching the mechanics for.

### 5.6 Calibration

`cal_offset[5]` in NVS. Procedure (web UI Calibrate page or CLI): home the
column; nudge ±1 / ±10 µsteps live until the blank card hangs flat against the
bezel lip and the next card is fully retained; save. Then step through indices
0..49 to confirm the mapping. Per-column `hall_to_hall` measured count is shown
for diagnostics (expect 8242–8243).

### 5.7 Idle current `[default]`

Drivers stay enabled at rest, relying on TMC2209 standstill reduction. Phase 1
bench test: with EN released, does a loaded drum creep over 10 min? If it holds,
`motion.en_idle_off` can default to true (cooler motors). Until then, false.

---

## 6. Frame layer

A **frame** is five ring indices. `frame.show(f)` computes each column's
forward distance and starts all five moves simultaneously (the Swan's columns
flip together). A new frame while moving simply replaces targets; forward-only
means an "earlier" target becomes a wrap, which is acceptable.

Clock moves start on the tick and wraps land late. Countdown moves are
scheduled to land on the tick (§7.3). Both are per-mode config.

---

## 7. Modes

Arbitration: exactly one active mode owns the frame. Countdown overrides clock;
message overrides clock with a dwell; FAULT overlay never changes the mode.

### 7.1 Clock

Layout (Q2, **confirmed**): col 1 = AM/PM (blank in 24 h) · cols 2–3 = hours ·
cols 4–5 = minutes. 12 h: hours 1–12, col 2 blank below 10. 24 h: leading zero.
Renders at second 0 of every local minute; DST handled by the TZ string.

Why col 1 and not col 5: the physical 3 + 2 grouping forces HH | MM across
the band (cols 2–3 | 4–5), which leaves exactly one spare column, and it is
col 1. `FIRMWARE_HANDOFF.md` §2 mentions "col 5 carries AM/PM" but in the same
paragraph states the digit-to-column assignment is firmware's choice; AM/PM
on col 5 is only possible by shoving minutes across the band ("104 | 5PM").
Col 1 it is, unless Nico overrides.

Until time is valid: after homing, show all blank; if SNTP has not synced
within `WIFI_GLYPH_GRACE_S = 15`, show the WiFi glyph on the **centre column**
(col 3), blanks elsewhere — per the handoff, which is DECIDED there. A WiFi
drop after a successful sync keeps free-running time and does **not** show the
glyph.

### 7.2 Message

Five tokens (§4). Dwell `msg.dwell_s = 600` then return to the previous mode;
`hold=true` keeps it until changed `[default]`.

### 7.3 Countdown (108:00)

- Total 6480 s. Display is **MMM:S0** — minutes in cols 1–3, tens-of-seconds in
  col 4, col 5 fixed at 0. The ones-of-seconds can never move: a decrement is
  49 flips and a second is 1 s. Decided; do not revisit.
- Frame updates on every 10 s boundary: `shown = floor(remaining / 10) * 10`.
  Each boundary may spin col 4 (49 flips) and, every minute, a minutes column.

**The display is self-sufficient.** Everything — entering the Numbers,
switching modes, calibration — is done from the display's own web UI on any
phone or PC on the LAN (`lost.local`). No broker, no Pi, no cloud is required
for any feature. MQTT and the terminal prop are optional peers that use the
same command set; if they are absent, nothing is missing.

**The countdown is a deadline, not a timer.** State is `{state, target, set_by,
seq}` where `target` is a UTC epoch timestamp, held by the display. Remaining
time is always `target − now` from NTP-synced time (free-running clock if WiFi
drops). Reasons:

- The deadline is written to NVS when set (one write per set, not per tick),
  so a power cycle mid-countdown resumes from it — with or without a broker.
- When MQTT is configured, the same state is also published **retained** on
  `swan/countdown`, so a **Swan terminal prop** (Pi 4, CRT, Apple II keyboard —
  separate, optional build) can mirror the display and set the deadline too.
  Both render from the same deadline via NTP; whoever set it last wins. No
  master.
- Cues (4:00, 1:00, zero) are derived from the deadline; audio plays on the
  display.

Commands (§10.2a): `countdown.execute {numbers}` validates the Numbers then
sets `target = now + 6480` (start or mid-run reset, exactly as the show);
`countdown.start` / `reset` do it without the ritual; `countdown.set_target
{epoch}` lets the terminal set an explicit deadline; `cancel` → idle. Wrong
Numbers → `rejected` on `swan/event`.

**Landing on the tick.** Because the terminal screen is an exact reference,
countdown moves are scheduled to *land* on the 10 s boundary: the scheduler
knows each column's move duration (flips × per-flip time + ramp) and starts
the frame early by the longest column's duration, so the whirl ends as the
screen rolls over. `countdown.land_on_tick = true` by default; the clock keeps
`clock.land_on_tick = false` (start on the minute, nothing to sync to). Both
configurable.

- Cues (Q4, defaults): at remaining = 240 s play `warn_4min`; at 60 s
  `warn_1min`; at 0 s `system_failure`. Exact sounds and whether they loop are
  Nico's.
- At zero (Q4, defaults): show 000:00; after `ZERO_HOLD_S = 3` all five columns
  spin continuously at alarm speed for `ALARM_SPIN_S = 6`, then land on the
  **reveal frame** (five glyphs, `countdown.reveal[5]`, config — TBD by Nico,
  currently unset). Audio loops for at most `FAILURE_LOOP_S = 60`. The display
  stays on the reveal until the mode is changed or the Numbers are entered
  again (which restarts at 108:00). No auto-return to clock unless
  `failure_timeout_s > 0`.
- **"?????" state.** The ring carries a `?` glyph (slot 33 in the current
  manifest; look it up by role `question`, never by index). The show's
  full-display `?????` state is a named preset: `preset.set {name: "qmarks"}`
  → all five columns to `?`. Other presets: `blank`, `reveal`, `wifi`.

### 7.4 Boot / no-signal

Covered in §7.1. Column FAULT display policy is `[Q5]`.

---

## 8. Time

SNTP via `esp_sntp`, servers configurable (default `pool.ntp.org`). Timezone as
a POSIX TZ string, settable in the web UI; **default US Pacific**
(`PST8PDT,M3.2.0,M11.1.0`, DST handled by the string). `time_valid` set after
first sync.
No RTC: free-run on WiFi loss (drift is tens of ppm — seconds per day, fine
for a wall clock that resyncs when WiFi returns).

---

## 9. Audio

- Assets: PCM WAV, 16-bit mono, 22 050 Hz (16 000 acceptable), stored in the
  LittleFS `/audio/` directory. Nico supplies the files; firmware ships
  synthesized placeholder beeps so the pipeline can be tested.
- Player task streams one file at a time to I2S via DMA; a new cue preempts
  the current one; `loop` and `max_loop_s` per cue.
- Volume 0–100 as software gain, mute switch, quiet hours `[Q8]` (default off).
- Cue table: `warn_4min`, `warn_1min`, `system_failure`, optional `ui_execute`,
  `ui_reject`.

---

## 10. Network and integration

### 10.0 Independence
The web UI is the primary control surface and is complete on its own: the
Numbers terminal, mode switching, message entry, calibration, settings, OTA.
MQTT is **off until configured** and the firmware never waits on it. The
terminal prop is a peer, not a dependency.

### 10.1 WiFi
STA mode; dual-band is automatic (follows the SSID). Provisioning: if no
credentials, start SoftAP `LOST-Swan-xxxx` with a captive portal page to enter
SSID/password, then reboot `[default]`. mDNS hostname `lost` → `lost.local`.

### 10.2 Web UI
Static files (vanilla HTML/CSS/JS, no framework, Swan-terminal aesthetic) in a
LittleFS partition, served by `esp_http_server`. WebSocket `/ws` pushes the full
state JSON on change plus a 1 Hz heartbeat. REST under `/api/`.

Pages:
- **Terminal** — the Numbers prompt + Execute, live mirror of the five columns,
  mode indicator, remaining time.
- **Modes** — one-tap switching between clock / message / countdown at any
  time; message has a glyph picker for five slots, dwell and hold. Every
  control here is a §10.2a command, identical to what MQTT accepts.
- **Calibrate** — per-column ±1/±10 nudge, save, rehome, index walk, test spin,
  hall_to_hall readout.
- **Settings** — WiFi, MQTT, TZ (default US Pacific), 12/24 h, audio, quiet
  hours, speeds, idle current policy, fault policy, **Ring** (view the loaded
  table, upload a new `ring.json`).
- **Diagnostics** — per-column counters, RSSI, heap, uptime, reset reason, log
  ring buffer.
- **Update** — OTA upload.

Auth `[Q7]` (default: none on LAN).

### 10.2a Command bus — one command set, every transport

Nico: the web UI must switch modes on the fly, and a separate **Swan terminal
prop** will drive the display, most simply over MQTT. So there is exactly one
command dispatcher, and every control path feeds it: web UI (REST/WebSocket),
MQTT, serial CLI, the physical button, and HA. MQTT is the canonical external
API; anything the UI can do, a prop can do with a publish.

| command | payload | effect |
|---|---|---|
| `mode.set` | `clock` \| `message` \| `countdown` | switch mode immediately; countdown switches to an idle 108:00 until started |
| `message.set` | `{tokens:[5], dwell_s?, hold?}` | show five glyphs |
| `countdown.execute` | `{numbers:"4 8 15 16 23 42"}` | the ritual: validates, then starts or resets to 108:00; wrong numbers → `rejected` |
| `countdown.start` / `reset` / `cancel` | — | bypasses the Numbers (HA, automations) |
| `countdown.set_target` | `{epoch}` | explicit deadline from the terminal prop; state is published retained on `swan/countdown` |
| `preset.set` | `{name}` | `qmarks` (?????), `blank`, `reveal`, `wifi` |
| `display.frame` | `{indices:[5]}` or `{tokens:[5]}` | raw frame, for props and tests; does not change mode |
| `audio.volume` / `audio.mute` / `audio.play` | `{value}` / `{on}` / `{cue}` | |
| `motion.rehome` | `{column?}` | |
| `motion.cal` | `{column, delta}` / `{column, save}` | calibration nudges |
| `clock.format` | `{h24}` | |
| `system.reboot` | — | |

MQTT: `swan/cmd/<command>` with a JSON payload (a bare string is accepted
where the payload is a single value). Every command publishes its result on
`swan/event` (`ok`, `rejected`, `fault`) and the new state on `swan/state`.
The terminal prop can either send the typed numbers via `countdown.execute`
(validation stays in the display firmware, one source of truth) or validate
locally and send `countdown.start`. Both are supported.

### 10.3 MQTT + Home Assistant discovery
Base topic `swan/` (confirmed). Retained `swan/state` JSON; retained
`swan/countdown` deadline state (§7.3); LWT on `swan/availability`; results on
`swan/event`. Commands are the §10.2a set under `swan/cmd/…`. The terminal
prop is a plain MQTT peer: it subscribes to `swan/countdown` and `swan/state`
and publishes to `swan/cmd/…`.

Discovery under `homeassistant/<component>/swan/<object_id>/config`, one device
("LOST Swan Timer"). Proposed entities:

| entity | type |
|---|---|
| mode | select: clock / message / countdown |
| message | text (five tokens) |
| execute / cancel / rehome | button |
| volume | number 0–100 |
| mute, 24h | switch |
| state, remaining_s | sensor |
| fault, time_valid | binary_sensor |
| resync_minor/major per column, flips_total | sensor (diagnostic) |

### 10.4 OTA
Upload page → `esp_ota` with rollback enabled
(`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`); new image must mark itself valid
after a successful boot + home. Motion is held during flash writes regardless
of the IRAM-safe ISR.

---

## 11. Config (NVS) — schema sketch

```
wifi.ssid / wifi.pass
mqtt.host / mqtt.port / mqtt.user / mqtt.pass / mqtt.base / mqtt.enabled
time.tz                  default PST8PDT,M3.2.0,M11.1.0
time.ntp                 default pool.ntp.org
ring                     ring.json in LittleFS (not NVS); compiled fallback
clock.h24
motion.cal[5]            int32 µsteps
motion.flaps_s_normal    default 15
motion.flaps_s_alarm     default 25
motion.flaps_s_home      default 8
motion.accel             default 82000
motion.hall_tol          default 41
motion.en_idle_off       default false
motion.fault_policy      [Q5]
audio.volume / audio.mute / audio.quiet_start / audio.quiet_end
msg.dwell_s              default 600
countdown.reveal[5]      ring indices, unset until Nico picks
countdown.land_on_tick   default true
clock.land_on_tick       default false
countdown.zero_hold_s    default 3
countdown.spin_s         default 6
countdown.failure_loop_s default 60
countdown.failure_timeout_s default 0
net.auth_pass            [Q7]
```

---

## 12. Diagnostics

Per column: `flips_total`, `revs`, `resync_minor`, `resync_major`, `faults`,
`last_hall_err`, `hall_to_hall`. Global: heap, RSSI, uptime, reset reason,
firmware version. Log ring buffer (last ~200 lines) readable from the UI; full
log on the USB console. Task watchdog on the motion control task and the main
loop.

---

## 13. Serial CLI (bring-up)

`pins` · `hall` (live levels) · `en 0|1` · `step <col> <n>` · `home <col>|all`
· `go <col> <index>` · `spin <col> <flaps_s> <seconds>` · `revs <col> <n>`
(measure hall_to_hall over n revolutions) · `cal <col> <±µsteps>` · `save` ·
`frame a b c d e` · `mode …` · `stats` · `wifi …` · `mqtt …` · `audio play <cue>`
· `reboot`.

---

## 14. Testing strategy

- **Host unit tests** (CMake native build, no IDF): ring arithmetic, `T(i)`
  rounding and wrap recomputation, clock rendering for 12/24 h and DST edges,
  countdown schedule (every 10 s boundary, cue timing, zero choreography), mode
  arbitration. These run on the dev machine and in CI.
- **Simulator**: a browser page rendering the five columns from the same frame
  logic, fed by a mock WebSocket — lets the UI and modes be exercised before
  the drums exist.
- **Bench checklists** per phase (§15), executed by Nico with the CLI.

### 14.1 Phase 1 bench checklist (one module)
1. Flash, open the USB console, `pins`.
2. `hall` — wave the magnet past the sensor; confirm polarity and active level.
3. `step 0 200` — confirm the drum turns in the **ascending** direction. If not,
   move that driver's DIR tie to the other rail. DIR is tied per driver, so
   this is a per-column fix; coil order on the JSTs does not need to match.
4. `home 0`, then `revs 0 10` — record hall_to_hall; expect 8242 ± a few.
   A consistently different value means the microstep setting or gear teeth
   differ from §3.
5. `spin 0 …` sweep 10 → 25 flaps/s with flaps installed; note the rate where
   flaps stop clearing cleanly. That number sets `flaps_s_alarm`.
6. Edge repeatability: 20 revolutions, report max |err|. Sets `hall_tol`.
7. `en 0` for 10 min with a loaded drum; does it creep? Decides `en_idle_off`.
8. `cal 0 ±n` until blank sits right; `save`; `go 0 <i>` for every index.

---

## 15. Build plan

Each phase ends with a flashable build and a bench checklist.

1. **Skeleton + motion core** — IDF project, pin map, config/NVS, CLI, step
   ISR, axis FSM, homing, edge verification, calibration; host tests for §3/§5.3.
1.5. **Simulated-axis suite + CI** — the control core extracted pure
   (`axis_control.cpp`) and driven on the host against a modeled drum
   (272000/33 µsteps/rev, configurable Hall window, jitter, slip, wrong
   gearing): homing from any start angle incl. inside the magnet window, the
   full 50×50 `go` matrix with mid-move re-basing, slip classification
   (minor/major/fault→auto-rehome), cal normalisation, the 8369-drum
   signature, mailbox ordering.  GitHub Actions (`.github/workflows/ci.yml`)
   runs the host suites natively on ubuntu and builds both board maps in
   Espressif's official `espressif/idf:v5.5.5` image — **Linux CI is the
   reliability source of truth**; the Windows SAC-retry is local convenience.
   Sync contract: `docs/MOTION_SYNC.md`.
2. **Frame + modes + simulator** — clock, message, countdown scheduler, reveal
   choreography, time service.
3. **Web UI** — Terminal, Modes, Calibrate, Settings, Diagnostics, Update.
4. **Integration** — MQTT + HA discovery, OTA with rollback, provisioning, mDNS.
5. **Audio** — player, cue table, countdown cue wiring, placeholders.
6. **Hardening** — fault policy, watchdogs, boot stagger, soak mode (run N
   wraps, log resyncs), power-loss behaviour, documentation.

Toolchain `[Q1, default]`: ESP-IDF 5.5.x (latest patch), C++17, `idf.py`.
Arduino-as-IDF-component is the fallback if a specific Arduino library is
worth it. ESPHome is not planned (custom Terminal UI is the reason).

---

## 16. Open questions — all answered 2026-08-21

| # | question | answer |
|---|---|---|
| Q1 | Toolchain | ESP-IDF 5.5.x native, C++17, `idf.py` from a terminal (default accepted) |
| Q2 | Clock layout / TZ | Layout as specified, 12 h default. **TZ settable in the web UI, default US Pacific.** |
| Q3 | Ring | Same characters on all drums; cols 4–5 use a different colour scheme; **order and placement still fluid** → ring is a runtime data file (§4). Manifest arrives via the ref zip. |
| Q4 | Countdown | All defaults: HA direct start yes, cancel yes, cues at 4:00/1:00/0:00, zero choreography as §7.3 |
| Q5 | Column fault | Park on blank, others continue |
| Q6 | Button | Yes — GPIO28, press = Execute, hold = rehome |
| Q7 | Web UI | No password on LAN; OTA via upload page |
| Q8 | Audio | Quiet hours off; default volume 70 |
| Q9 | Repo | GitHub from day one, MIT |
| — | Control surfaces (added) | Web UI switches modes on the fly; an external **terminal prop** drives the display over MQTT → single command bus (§10.2a) |

Standing defaults, all accepted: moves start on the tick, staggered boot
homing, 10-minute message dwell, 15/25 flaps/s, drivers enabled at rest until
the bench test, ¼-flap Hall tolerance, `swan/` base topic, `lost.local`, DIR
tied at each driver.

---

## 17. Firmware decision log

Append here when a `[Q]` is answered or a default is overturned, with the
reason, so nothing gets re-litigated.

- 2026-08-21 — Spec v0.1 drafted from BUILD/README v6, BOM, and the ring
  freeze. Ascending ring, MMM:S0 countdown, standalone TMC2209 at 1/16, A3144
  halls, ESP32-C5 as sole controller are carried over as locked.
- 2026-08-21 — Board question reopened: a Seeed XIAO ESP32-C5 was ordered
  (11 header GPIOs; fits only with four back-pad solders). Options documented
  in §2.0–2.4: XIAO, ESP32-C5-DevKitC-1-N8R8 (recommended), Pi Pico 2 W
  (plan B). Shift registers rejected. DIR tied per driver on every option.
  Strapping-pin list corrected from Espressif's v1.2 DevKitC-1 guide
  (2, 3, 7, 25, 26, 27, 28); an earlier "~22 header GPIOs" claim for the
  DevKitC-1 was wrong — it is 19.
- 2026-08-21 — **ESP32-C5-DevKitC-1-N8R8 ordered** (third-party Amazon
  listing). Becomes the primary target; §2.2 pin map is the Phase 1 map.
  Open risk: chip revision. If v0.1 arrives, fall back to the XIAO map (§2.1)
  rather than wait. PCB-antenna module: mount away from the aluminium
  faceplate and check RSSI in Phase 1.
- 2026-08-21 — **All open questions answered; spec cut to v1.0.** Defaults
  accepted except: TZ default US Pacific and UI-settable; ring treated as
  fluid runtime data (`ring.json`, per-column capable, regenerated from the
  manifest by `tools/ringgen.py`, no direct index references in code);
  single command bus with MQTT as the canonical external API because a
  separate Swan terminal prop will drive the display.
- 2026-08-21 — **Phase 1 scaffold review.** Reference-file conflicts raised
  by the Claude Code session, resolved here: (1) gear teeth — 85/33 stands,
  MECHANICAL_README §3 narrative is stale (68/26); the physical drum is
  confirmed by `revs 0 10` (8242 vs 8369). (2) DIR stays tied per driver; the
  handoff's per-column `invert_dir` GPIOs are superseded by §2.0–2.4. (3)
  WiFi glyph moves to the **centre column** (handoff DECIDED). (4) AM/PM stays
  on col 1 — the handoff delegates the assignment to firmware and col 5 is
  geometrically impossible with HH | MM across the band. (5) Phase
  accumulator not adopted; edge-relative targeting is strictly stronger and
  a host test guards it. (6) `components/hal` → `components/swan_hal`; NVS
  keys shortened to the 15-char limit with a mapping table.
- 2026-08-21 — **Countdown becomes a deadline** (`target` epoch, retained on
  `swan/countdown`), so the Swan terminal prop and the display render from
  one source without a master, and a power cycle resumes. Countdown moves
  land on the tick; clock moves start on it. `?????` preset added.
- 2026-08-21 — Nico: the display must work **independently of the prop
  computer**; the Numbers are entered from a phone or PC. Spec tightened:
  web UI is the complete primary surface, MQTT off until configured, deadline
  persisted in NVS so resume works without a broker, terminal prop optional.
- 2026-08-21 — **Q1 confirmed in practice: ESP-IDF v5.5.5, `idf.py`.** Toolchain
  installed and Phase 1 builds clean for both board maps; host tests pass.
  Windows caveat worth recording: `install.sh`/`export.sh` do NOT work here.
  ESP-IDF dropped MSys/Mingw support at v4.0 and `install.sh` refuses with
  "MSys/Mingw is not supported"; the working path is `install.ps1` /
  `export.ps1`. Recorded in README.md.
- 2026-08-21 — **Phase 1 accepted.** Three follow-ups executed before Phase 2:
  (1) repo pushed to GitHub with CI — host suites native on ubuntu, both board
  maps in `espressif/idf:v5.5.5` Docker; **Linux CI is the reliability source
  of truth**, the Windows Smart-App-Control retry in `test-host.ps1` is local
  convenience, and host builds stay free of static libraries while `ar.exe`
  is blocked there. (2) **Phase 1.5**: the motion control core was extracted
  pure (`components/motion/axis_control.{h,cpp}`; `motion.cpp` is now an IDF
  shell with no control logic) so the *real* control tick and the *real*
  ISR helpers run on the host against a modeled drum — spec §15 item 1.5
  lists the assertions. (3) `docs/MOTION_SYNC.md` records the ownership /
  atomics / critical-section / IRAM-DRAM contract; changes to motion must
  keep it true.
- 2026-08-21 — **Adversarial review of Phase 1.5 (40 agents, findings verified
  by independent refuters); outcomes:**
  - **Critical, fixed:** the §5.3 recompute rule as previously written never
    terminated when `cal_offset + T(dest)` exceeded one revolution — every
    Hall edge re-added a revolution to the target (any cal > 164 made index 49
    unreachable; mid-range cal broke half the ring). Two verifiers reproduced
    it by simulation. Fix: the edge-anchor offset is reduced modulo one
    nominal revolution (`edge_anchor_offset` in motion_math.h); §5.3 rewritten;
    regression tests cover cal ∈ {200, 4000, 8142, 8241} and a full-cal sweep
    of the reduction invariant. The bug predates the pure-core refactor.
  - **Major, fixed:** a Stop drained during a homing pass either spuriously
    faulted (Seek) or falsely completed homing at the wrong position (Settle).
    Stop now aborts homing to UNHOMED honestly, and cancels a pending
    staggered home. Pinned by test.
  - **Test gaps, closed:** the EdgeVerdict::Fault classification branch was
    never exercised (a +200 slip faults via the missed-edge timeout instead —
    both are correct paths, but only one was covered); a new negative-slip
    test hits classification directly. Mid-move re-basing was never verified
    under a disturbed drum (all slip tests were open-loop); a new test injects
    slip during a wrapping `go` and asserts the landing is correct relative to
    the drum. Home with `delay_ticks = 0` parked the axis silently; now
    clamped to 1.
  - **Decisions:** open-loop stepping (`step`/`spin`) may clear a latched
    FAULT — it is the bench un-jamming tool (spec §13); `go` from FAULT stays
    rejected so nothing false is displayed. `enter_fault` no longer hard-stops
    before the auto re-home; the ramp converges onto homing speed (smooth,
    forward-only-safe). Resuming the interrupted frame after a successful
    re-home is the Phase 2 frame scheduler's job — the core publishes the
    homed event; noted so §5.4's "resume the current frame" lands there.
  - Docs corrected to match code (MOTION_SYNC critical-section count,
    `dda_tick` inlining guarantee, README CI trigger wording), and the honest
    scope of the host-side mailbox verification recorded in MOTION_SYNC.
