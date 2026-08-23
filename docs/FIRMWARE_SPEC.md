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
| MCU | **ESP32-C5-DevKitC-1-N8R8**, board V1.2 — **on the bench since 2026-08-23**. Pin map in §2.2. | 8 MB flash, 8 MB PSRAM, USB-Serial-JTAG + USB-UART bridge. **Chip revision v1.2, verified** (§2.0) — production silicon; the revision risk is closed. XIAO ESP32-C5 is the spare / project-#2 board; Pico 2 W remains plan B. |
| Motors | 5 × NEMA 17, 17HS4401-class, 1.8° (200 full steps/rev) | `VERIFY` step angle on the motors actually bought. |
| Drivers | 5 × TMC2209 modules, **standalone (no UART)** | MS1 = MS2 = high → 1/16 microstep, internal 256 interpolation. `VERIFY` against the vendor's silkscreen/doc; a different default pull changes every motion constant. Vref set for ~1.1–1.2 A RMS. Standstill current reduction left enabled. |
| Drive | 33T pinion on motor → 85T gear cut into spool disc rim | Ratio **85/33 = 2.5758:1**, not 2.6. `VERIFY` teeth counts against BUILD/README v6. |
| Rotation | **One direction only** (ascending ring). Reverse is mechanically forbidden (flaps jam on the bezel lip). | **DIR is tied to a rail at the drivers** (no GPIO — the XIAO can't spare one; DJ Harrigan's build does the same). Which rail is decided in Phase 1 bench step 3. EN is a single ganged GPIO. |
| Home sensor | 5 × A3144 digital Hall (TO-92), Ø6×3 N35 magnet at R52 on the idler disc, one per column | A3144 supply is 4.5–24 V → must be fed **5 V**; output is open-collector, pull it up to **3V3** (10 k) so the GPIO sees 3.3 V logic. `VERIFY` the hall JST carries 5 V. Output is active-LOW when the magnet is present (`VERIFY`). One operate edge per spool revolution. |
| Audio | MAX98357A I2S mono amp + 40 mm 4 Ω 3 W speaker | 3 GPIOs (BCLK, LRCLK, DIN). Gain pin left at default 9 dB. `VERIFY` SD/shutdown pin handling on the module. No hardware volume → software gain. |
| Power | 12 V 6 A PSU → drivers; 12→5 V buck → logic, halls, amp | All five spinning ≈ 4–5 A on 12 V. |
| Status | Onboard LED on GPIO27 on either C5 board | XIAO: single yellow LED, active low → blink patterns. DevKitC-1: WS2812 RGB → colour-coded status. |
| Button | One user button `[Q6]` | Wired in parallel with the onboard BOOT button (GPIO28, active low) on either C5 board. Costs no GPIO. Must not be held low at reset except to enter the bootloader. |

### 2.0 Board decision — DevKitC-1-N8R8 (**arrived, verified 2026-08-23**)

**The chip-revision risk is closed.**  The board on the bench is an
ESP32-C5-DevKitC-1 **V1.2** (PCB silkscreen) carrying **ESP32-C5 revision
v1.2** — production silicon, not an engineering sample.  `esptool chip_id` and
the second-stage bootloader agree:

```
Chip is ESP32-C5 (revision v1.2)
Features: Wi-Fi 6 (dual-band), BT 5 (LE), IEEE802.15.4, Single Core + LP Core, 240MHz
Crystal is 48MHz          USB mode: USB-Serial/JTAG      BASE MAC: 10:bd:a3:dd:a8:e8
I (22) boot: chip revision: v1.2
I (22) boot: efuse block revision: v0.3
ESP-ROM:esp32c5-eco3-20250704
```

Console is on **COM3**, USB-Serial/JTAG.

Worth keeping in mind for future IDF bumps: an IDF image carries a *supported
chip revision range*, and the bootloader **aborts rather than warns** if the
chip is outside it — `Image requires chip rev <= …`.  That is an IDF-version
problem, never a board fault.  This build reports:

```
I (257) efuse_init: Min chip rev: v1.0   Max chip rev: v1.99   Chip rev: v1.2
```

so v1.2 sits inside the window and there is no abort.  ESP-IDF v5.5.5 also
offers `CONFIG_ESP32C5_REV_MIN_102`, i.e. it knows v1.2 as a shipping
revision.

### 2.0.1 Board options as originally weighed

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

**EN is ganged and cannot be split.**  One GPIO (6) drives all five TMC2209 EN
pins, and the only spare non-strapping GPIO on this map is **24** - one, not
four.  So there is no per-column de-energize and there will not be one:
stopping a column stops it *stepping*, while its coils keep holding TMC2209
standstill current.  Releasing EN is the only true de-energize and it takes the
whole display with it.  The fault escalation in §5.8 is written around that
fact rather than pretending otherwise.

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

Transition costs on the **descending** v3 rings (cost one way + cost the other
= 50).  Ring A is columns 1–4, ring B is column 5 with its two digit blocks;
see §4.  All measured from the manifests, not estimated.

| transition | ring A | ring B | time @ 20 flaps/s (ring A) |
|---|---|---|---|
| countdown decrement (any digit −1) | **1** | **1** | 0.09 s |
| clock increment (any digit +1) | 49 | 24 | 2.45 s / 1.22 s |
| digit 0 → 9 (wrap) | 41 | **16** | 2.05 s / 0.84 s |
| tens-of-seconds 0 → 5 (minute rollover) | 45 | — | 2.25 s |
| blank → wifi glyph | 1 | n/a — no wifi on ring B | 0.09 s |

The decrement being one flip on **every** column is the point of the redesign:
it is the show's single-flap tick, and it is what makes live seconds possible
(§7.3).  The price is that clock increments became the expensive direction,
which §7.1's granularity setting exists to manage.

---

## 4. Ring (50 positions — two *fluid* data tables, not code)

**v3, descending, frozen 2026-08-22.**  Two rings ship, from two manifests in
`docs/ref/`:

| ring | columns | manifest | contents |
|---|---|---|---|
| **A** | 1, 2, 3, 4 | `manifest_cols1234.json` | blank @0, wifi @1, 36 glyphs @2–37, PM @38, AM @39, digits @40–49 (`slot = 49 − digit`) |
| **B** | 5 | `manifest_col5.json` | blank @0, 14 glyphs, digits @15–24 (`slot = 24 − digit`), `?` @25, 14 glyphs, digits @40–49 (`slot = 49 − digit`) |

**Descending.** One forward flip *decrements* the displayed digit.  The drum
turns one way, so a transition costs `(target − current) mod 50` flips; making
the ring descend turns every countdown decrement into a single flip.  Clock
increments become the expensive direction — accepted deliberately, and managed
by `clock.granularity_min` (§7.1).

**Column 5 carries two digit blocks, 25 slots apart.**  That drops its 0→9
wrap from 41 flips to 16, which is what makes live seconds affordable (§7.3).
It is paid for by dropping AM/PM (rendered on column 1), the wifi glyph
(column 3) and seven glyphs; all nine on-screen canon glyphs and `?` survive.

**Consequence — a digit does not have one slot.**  Every lookup takes the
column's *current* slot and returns the nearest match in the **forward**
direction.  Column 5's physical position is therefore genuinely not predictable
from the displayed digit: showing `7` may mean slot 17 or slot 42.  That is
correct behaviour, not a fault — `docs/BRINGUP.md` says so where the
calibration walk would otherwise raise the alarm.

Design consequences carried over from Q3 and still in force:

- The ring tables are a **runtime file**, `data/ring.json` in LittleFS, loaded
  at boot, with compiled-in copies as fallback.  Replaceable from the web UI
  (Settings → Ring → upload) without reflashing.
- `tools/ringgen.py` generates `ring.json` **and** the compiled fallback header
  from the two manifests.  Nobody edits either output by hand.
- Each column may carry its own table (`columns[i].ring`; `slots` accepted as
  an alias).  Columns 1–4 use the shared table; column 5 always carries its own.
- **No code references a ring index directly.**  Renderers ask for roles —
  `blank`, `digit(n)`, `am`, `pm`, `wifi`, `question` — and the message parser
  asks by name.
- **Role coverage is validated at load, not at render.**  Every column must be
  able to render blank, `?` and digits 0–9; column 1 must have AM/PM; the
  centre column must have the wifi glyph.  A table that fails is rejected and
  the compiled fallback stays active, so a bad upload surfaces at boot rather
  than as a blank column mid-show.  Column 5 legitimately lacks AM/PM and wifi
  and is never asked for them.
- Per-column calibration offsets live in NVS, independent of the tables, so a
  ring change never invalidates calibration.
- The Calibrate page's index walk shows the *expected* character beside each
  stop, which is how a table/drum mismatch — or a swapped drum, since there are
  now three part numbers — is caught in seconds.

Rules:
- A move from slot *c* to slot *t* always costs `(t − c) mod 50` flips, and the
  result is never negative: no move can ask for a reverse DIR.
- Column count and ring size are config, so the second split-flap project
  reuses the codebase.  A table of any size other than 50 is rejected — the
  drums are physical and `T(i)` is compiled for that geometry.
- Message tokens: ring names from the table, `_` for blank, or `#n` for a raw
  index.  A name may match two slots on column 5; the nearest forward wins.
  `#n` is always exact.

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

### 5.8 Fault classification and escalation

Two fault causes want opposite responses, and treating them alike drives a
stepper into an obstruction for 7.5 s at a time.  The discriminator is
**whether the Hall edge arrived**:

| cause | signature | what it means | response |
|---|---|---|---|
| `no_hall` | a cold homing pass ran 1.2 revolutions with **no edge at all**, or the hall never released | sensor, magnet, wiring, or a Hall that never asserts | retry (the drum is almost certainly turning freely, so a re-home costs only time) |
| `slip` | the edge **arrived**, more than one flap from where it was expected | the drum turned but lost registration - a card catching, a pinion skipping | retry (a re-home is the recovery, and Phase 1.5 pins this behaviour) |
| `jam` | an expected edge **never came** while the motor kept stepping (1.5 revolutions past the last edge), or a homing pass timed out on a column that *had* previously seen an edge | mechanical resistance - the drum has stopped and the motor has not | **stop at once, no retry** |

Nico asked for two causes; there are two *responses* and three causes, because
slip is retryable for a different reason than a dead sensor is, and folding it
into `jam` would regress the recovery the simulated-axis suite has pinned since
Phase 1.5.  `components/motion/fault_policy.h` states the rule; it is pure and
host-tested (`test/host/test_fault_policy.cpp`).

**Escalation** is a whole-machine judgement, evaluated when a fault is raised:

| condition | escalation | effect |
|---|---|---|
| single column, `no_hall` or `slip` | `park_column` | park that column, the rest of the display keeps running |
| single column, `jam` | `stop_column` | stop **that** column immediately, no retry |
| **two or more** columns faulted | `drop_enable` | release EN for all five |
| **any** fault while a column is in a high-speed open-loop spin | `drop_enable` | release EN for all five |

Two columns failing together is not two coincidences - it is power, the loom,
or the frame.  A fault during the alarm spin is the worst moment to keep
driving: five drums at 25 flaps/s can turn a fixable problem into a broken
gear, and the display is worth less than the mechanism.

**Hardware limit - EN is ganged.**  All five TMC2209 EN pins are on one GPIO
(§ 2.2), and the pin map has exactly **one** spare non-strapping GPIO
(GPIO24), so per-column de-energize is not possible and adding it would need
four more pins that do not exist.  Consequences, which are real:

- `park_column` and `stop_column` stop **stepping** a column.  Its coils still
  hold TMC2209 standstill current, so a jammed column is still energised
  against the obstruction, just not being driven further into it.
- `drop_enable` is the **only** true de-energize, and it necessarily takes the
  whole display with it.
- Therefore "stop everything" is the correct response to any signal that
  something structural may be wrong, and the firmware prefers it over trying to
  isolate a column it cannot actually isolate.

A fault **stays a fault** and stays visible until cleared by a `home`/`rehome`
command or by leaving maintenance mode.  Nothing times it out.

### 5.9 Per-column state and maintenance mode

**Per-column mode**, persisted in NVS (`col_mode`, a five-byte blob):

| mode | behaviour |
|---|---|
| `real` *(default)* | drives the hardware |
| `sim` | the same control core runs against a modelled drum (§ 5.10) |
| `disabled` | parked on blank, excluded from every frame, never homed, never retried, reported as **expected** rather than as a fault |

`disabled` is **set by the user and never inferred**.  No fault, timeout or
escalation writes it - a fault is a fault and stays visible until cleared;
turning it into configuration would silently hide a broken column for ever.
Set from the console (`col <n> real|sim|disabled`) or Settings -> Columns, both
through the one dispatcher (`motion.column`).

**Frames with a missing column: keep the mode running, leave the hole.**  A
clock missing one digit tells you more than a dark display, and halting the
whole show because one drum is out is a worse failure than the one it reacts
to.  The frame scheduler skips excluded columns everywhere - issuing, spinning,
lead-time budgeting and the settled test - and the renderers are untouched:
they keep producing five indices and never learn a column is missing.  One
refinement on the stated preference: a column being disabled is first **parked
on the home slot (blank)** while it is still homed and idle, so the hole reads
as a blank flap rather than a stale digit somebody would read as part of the
time.  The banner names which columns are out and why.

**Maintenance mode** (`maint` in NVS), a deliberate override, never inferred:

- Entered explicitly (`maint on`, Settings -> Maintenance, or
  `motion.maintenance`).
- **Survives a reboot** - pulling power mid-repair must not restart a countdown
  on top of your hands.  A boot in maintenance does **not** home and leaves EN
  released.
- Frame scheduling stops, modes suspend, cues are held (a system-failure alarm
  going off during a repair is exactly wrong), and automatic re-homing is off.
- A running countdown keeps its deadline - the deadline is absolute and a
  repair does not cancel it - it simply is not rendered or driven.
- **Manual commands work regardless of fault state**, which is the point: a
  suspect column is driven by hand from the Calibrate page.  `go` from FAULT,
  normally rejected so nothing false is displayed, is permitted in maintenance.
- Leaving re-arms everything and re-homes all five, because the drums have been
  moved by hand and nothing knows where they are.

The two compose, which is the intent: one real column and four simulated during
build-out; one disabled and four real during a repair.

### 5.10 Simulated axes (`sim`)

A simulated column runs the **real** control core, the real 1 kHz tick and the
real 50 kHz step ISR; only the Hall input is substituted, by a modelled drum
(`motion/sim_drum.h`).  One line in the ISR differs between real and simulated,
which is the point - modes, frames, ring, countdown, scheduler and the whole
web UI are the same code on the same path.

The model is honest where it matters: 272000/33 usteps per revolution as an
integer DDA (8242 + 14/33, so the 0.42 residue accumulates exactly as the real
drum does), the Hall window at the correct position, edge jitter, and therefore
a homing pass that takes the real ~7.5 s at homing speed.  It is division-free
so it is safe to call from the IRAM ISR.  Fault injection - `sim <col> slip
<+-usteps>`, `sim <col> miss <n edges>`, `sim <col> clear`, or
`motion.sim_fault` - exercises slip, missed edges, the classification above and
the recovery, on demand (the console spelling is `sim fault <col> slip <n>`).

**It must be impossible to mistake for real.**  Four independent surfaces say
so: an `ESP_LOGW` line at boot, a `motion.simulated` field in the state
payload, a permanently visible strip in the web UI and a chip on the
presentation terminal, and the per-column mode in `stats`.

**Never the default.**  The defaults of `ColumnConfig` are all-`real`,
not-in-maintenance, asserted at compile time by `static_assert` and again in
the host tests, so a fresh NVS boots real.  The simulation is compiled in under
`SWAN_SIM_AXES` (default ON during build-out); `-DSWAN_RELEASE=1` with
`SWAN_SIM_AXES` still on is a **`FATAL_ERROR` at configure time**, so a release
image cannot silently ship able to simulate.  CI builds both.

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
DST handled by the TZ string.

**Granularity.** The v3 rings descend, so a clock tick is the expensive
direction (§3): +1 costs 49 flips on ring A and 24 on ring B.  The displayed
minute is therefore floored to `clock.granularity_min`, **default 15**.
Renders happen when the floored *local* time changes — flooring in local time,
not UTC, so zones whose offset is not a whole multiple of the granularity
(India's +5:30 against a 15-minute grid) behave.

**`clock.granularity_min` must divide 60**: 1, 2, 3, 4, 5, 6, 10, 12, 15, 20,
30, 60.  A non-divisor does not tile the hour — at 7 the flooring steps
56 → 0 across the hour boundary, so the last window of every hour is four
minutes long and the display jumps.  Rejected at the API, and the UI offers
only the twelve.

Wear, **measured** by walking a full day through the real renderer against the
loaded ring (`components/modes/wear.cpp`, printed and asserted by
`test/host/test_wear.cpp`) — not estimated, and exposed live at `GET /api/wear`
so the Settings page shows the exact figure for any choice:

| granularity | col 1 | col 2 | col 3 | col 4 | col 5 | total flips/day |
|---|---:|---:|---:|---:|---:|---:|
| 1 min | 50 | 100 | 1,000 | 6,000 | 32,400 | **39,550** |
| 2 min | 50 | 100 | 1,000 | 6,000 | 14,400 | 21,550 |
| 3 min | 50 | 100 | 1,000 | 6,000 | 8,400 | 15,550 |
| 4 min | 50 | 100 | 1,000 | 6,000 | 5,400 | 12,550 |
| 5 min | 50 | 100 | 1,000 | 6,000 | 3,600 | 10,750 |
| 6 min | 50 | 100 | 1,000 | 6,000 | 2,400 | 9,550 |
| 10 min | 50 | 100 | 1,000 | 6,000 | 0 | 7,150 |
| 12 min | 50 | 100 | 1,000 | 4,800 | 2,400 | 8,350 |
| **15 min** *(default)* | 50 | 100 | 1,000 | 3,600 | 1,200 | **5,950** |
| 20 min | 50 | 100 | 1,000 | 2,400 | 0 | 3,550 |
| 30 min | 50 | 100 | 1,000 | 1,200 | 0 | 2,350 |
| 60 min | 50 | 100 | 1,000 | 0 | 0 | 1,150 |

Three things worth knowing from that table.  **Column 4, not column 5, is the
wear bottleneck** at 15 minutes (3,600 against 1,200): column 5 has the cheap
double-block increment while column 4 pays ring A's full 49.  At 20 minutes and
above column 5 never moves — the ones-of-minutes digit is always 0.  And the
cost is **not monotonic in the granularity**: 12 minutes (8,350) is worse than
both 10 (7,150) and 15 (5,950), because 10 keeps the ones-of-minutes digit on 0
while 12 walks it through 0,2,4,6,8 every hour.  That is precisely why the
figure is computed rather than tabulated — an interpolated lookup would hide it.

In 24 h the totals are identical at every granularity: column 1 stops moving
(blank all day, −50) and column 3 gains a flip per hour (+50).  Column 1's 50
flips a day are the two AM/PM changes, each a 25-slot move on ring A.

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
glyph.  The centre column is ring A, which carries the wifi glyph; ring B does
not, and the role validator (§4) enforces that the column that needs it has
it.

### 7.2 Message

Five tokens (§4). Dwell `msg.dwell_s = 600` then return to the previous mode;
`hold=true` keeps it until changed `[default]`.

### 7.3 Countdown (108:00)

- Total 6480 s.  **The seconds columns are frozen on `00` for the bulk of the
  run** and only come alive in the last `countdown.seconds_live_s` (default
  **240**, which is also when the 4-minute cue fires — the display waking and
  the warning sound are one moment).  Above the threshold the display is
  `floor(remaining / 60) * 60` rendered **MMM:00**; below it the resolution
  changes per `countdown.seconds_mode`:

  - **`seconds` (default)** — then **MMM:SS**, live one-second ticks.  Minutes
    in cols 1–3, tens-of-seconds in col 4, ones-of-seconds in col 5.  What the
    show does.
  - **`tens`** — then **MMM:S0**; column 5 stays on 0 all run.
  - **`minutes`** — never shows seconds; **MMM:00** for the whole run.

  The mode selects nothing but the display **step** (60 / 10 / 1 s); the
  renderer draws whatever value it is handed.  A step of 60 puts zeros in both
  seconds columns *by construction*, which is why the quiet phase provably
  cannot move them — there is no special case to get wrong.

  The transition is an ordinary step, not a mode switch: at 300 s the next
  value is 240 (**004:00**), and at 240 the next is 239 (**003:59**), so the
  display runs `004:00 → 003:59 → 003:58` with nothing jumping.

  This supersedes both the original "MMM:S0 throughout" and the v3 "MMM:SS
  throughout" — see the §17 entries.  **Measured cost of a full run**, walked
  through the real renderers against the real manifests by
  `test/host/test_wear.cpp` (these are its output, not estimates):

  | scheme | total | col 1 | col 2 | col 3 | col 4 | col 5 |
  |---|---:|---:|---:|---:|---:|---:|
  | `minutes` | **559** | 1 | 50 | 508 | 0 | 0 |
  | `tens` (freeze at 240) | **759** | 1 | 50 | 508 | 200 | 0 |
  | **`seconds` (freeze at 240, default)** | **1,359** | 1 | 50 | 508 | 200 | 600 |
  | `tens` throughout (pre-freeze) | 5,959 | 1 | 50 | 508 | 5,400 | 0 |
  | `seconds` throughout (pre-freeze) | 22,159 | 1 | 50 | 508 | 5,400 | 16,200 |

  So the default costs **1,359 flips a run against 22,159** — a 16× cut, and
  the show-accurate live seconds are now cheaper than the old MMM:S0 design
  was.  Note **column 3 is 508, not ~104**: the ones-of-minutes digit pays ring
  A's 41-flip 0→9 wrap on each of the ten borrows (100→099, 090→089, … 010→009)
  on top of 98 single flips.  It is the dominant cost of the quiet phase and
  the reason a `minutes` run is not free.

- **Timing budgets in the live window.**  A 1-flip tick takes ~0.1 s, comfortably
  inside the one-second window.  Two transitions do not fit their window and
  are handled by starting early and landing a little late rather than never:
  column 5's 16-flip wrap needs ~1.1 s at the default 15 flaps/s (~0.7 s at
  25), and column 4's 45-flip tens-of-seconds rollover needs ~3.0 s.  Column 4
  has ten seconds of slack — its value is valid for the whole next ten-second
  window — so a late landing there is invisible.  Column 5 catches up on the
  following tick, because a forward-only move simply extends when the target is
  replaced.

  Waking the seconds at the boundary is the single most expensive frame of the
  run: column 4 pays the 45-flip 0→5 borrow and column 5 the 16-flip 0→9 wrap,
  ~3.0 s and ~1.1 s at 15 flaps/s.  `countdown.land_on_tick` starts it early so
  it *lands* on 240 — which is what pre-rendering the next value exists for.
- Frame updates on every window boundary: `shown = floor(remaining / step) *
  step`, where step is 60 s in the quiet phase and then 10 s or 1 s per
  `countdown.seconds_mode`.  Note the floor: 108:00 is the idle face, and a
  *running* countdown holds it only for the start instant before rolling to
  107:00.

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

### 7.4 Boot / no-signal, and the FAULT display policy

Covered in §7.1.

**Column FAULT display policy (`[Q5]`, answered: park on blank, others
continue) - as executed, consistent with §5.8/§5.9:**

- A faulted column is **parked** (stepping stops); the other four keep running
  their mode.  The renderers are not told: they keep producing five indices and
  the frame layer drops the ones it must not command.
- Escalation can overrule "others continue": two or more columns faulted, or
  any fault during the alarm spin, **drops EN for all five** (§5.8).  That is
  a mechanical decision, not a display one.
- A `jam` says so in as many words - the drum stopped while the motor kept
  stepping - and is **not retried**.  `no_hall` and `slip` retry up to
  `REHOME_RETRIES = 3`.
- A **disabled** column (§5.9) is not a fault and is never reported as one.
  It is parked on blank and reported as configuration.
- A column whose position is **unknown** (`index = -1`, i.e. hunting, or after
  an open-loop spin) renders distinctly from one showing the blank flap -
  hatched and dimmed, with a pulse while homing.  They are different facts and
  looked identical until 2026-08-23.
- Whatever the state, it is named on a **persistent banner outside `<main>`**,
  visible from every page, with the retry attempt.  A homing pass is ~7.5 s and
  a column tries three times, so there is a ~30 s window from boot in which the
  display looks idle and is actually searching.

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
- **Presentation** (`/terminal.html`, phase 3.5) — a separate fullscreen page,
  not a tab: the Swan terminal face with a large countdown readout, an
  on-screen keypad for the Numbers, and the flap display docked small in a
  corner.  A toggleable CRT effect (scanlines, phosphor glow, slight barrel,
  subtle flicker) defaults **off** — it costs readability and the compositing
  is real work on a phone GPU — and key clicks are synthesized in WebAudio, so
  no samples ship.  Everything about it is browser-side: the ESP32 pays only
  the LittleFS bytes, and preferences live in `localStorage`, per browser,
  never in NVS.

  **This page is a candidate implementation of the terminal prop** (§7.3): a Pi
  in kiosk mode pointed at `lost.local`, on a CRT or a monitor.  It is
  therefore laid out for a kiosk screen and scales down, not designed for a
  phone and stretched up.  A prop built this way needs no MQTT and no second
  countdown implementation — it is the display's own page, so the deadline it
  renders is the display's by construction.

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
| `motion.column` | `{column, mode}` or `{all:true, mode}` | per-column `real` / `sim` / `disabled` (§5.9); persists to NVS |
| `motion.maintenance` | `true` \| `false` | enter/leave maintenance (§5.9); leaving re-homes all five |
| `motion.sim_fault` | `{column, kind, value?}` | inject `slip` / `miss` / `clear` on a **simulated** column (§5.10) |
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
ring                     ring.json in LittleFS (not NVS); compiled fallback,
                         generated from BOTH manifests by tools/ringgen.py
clock.h24
clock.granularity_min    default 15.  MUST divide 60: 1 2 3 4 5 6 10 12 15 20
                         30 60.  The displayed minute is floored to it; a
                         non-divisor does not tile the hour (§7.1)
motion.cal[5]            int32 µsteps
motion.flaps_s_normal    default 15
motion.flaps_s_alarm     default 25
motion.flaps_s_home      default 8
motion.accel             default 82000
motion.hall_tol          default 41
motion.en_idle_off       default false
motion.fault_policy      [Q5] - executed as §5.8/§7.4
motion.columns[5]        real | sim | disabled per column, default all real.
                         NVS key col_mode (5-byte blob).  `disabled` is set by
                         the user and NEVER inferred from a fault (§5.9)
motion.maintenance       default false.  Survives a reboot deliberately; a boot
                         in maintenance does not home and leaves EN released.
                         NVS key maint (§5.9)
audio.volume / audio.mute / audio.quiet_start / audio.quiet_end
msg.dwell_s              default 600
countdown.seconds_mode   default seconds.  What the display does INSIDE the
                         live window (§7.3): minutes = never shows seconds |
                         tens = MMM:S0 | seconds = MMM:SS
countdown.seconds_live_s default 240 (0..6480).  Seconds are frozen on 00 above
                         this and tick below it.  240 is the 4-minute cue, so
                         the display waking and the warning sound coincide.
                         NVS key cd_live_s
countdown.reveal[5]      ring indices, unset until Nico picks.  NOTE: an index
                         means a different character on col 5, whose ring
                         differs — set these by NAME when the web UI lands
countdown.land_on_tick   default true
clock.land_on_tick       default false
countdown.zero_hold_s    default 3
countdown.spin_s         default 6
countdown.failure_loop_s default 60
countdown.failure_timeout_s default 0
wifi.ssid / wifi.pass    STA credentials, set from the console (`wifi <ssid>
                         <pass>`) until captive-portal provisioning lands in
                         Phase 4.  NVS keys w_ssid / w_pass.
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

`col <n> real|sim|disabled` (or `col` to list) · `sim all|<n> [off]` ·
`sim fault <col> slip <±µsteps>|miss <n>|clear` · `maint on|off` ·
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
   **Delivered**, with WiFi STA and mDNS pulled forward from Phase 4 so the UI
   is reachable the day the board arrives.  Order executed: (a) a host dev
   server (`tools/devserver/`) serving the static assets and speaking the real
   `/ws` against the real ModeManager over simulated axes, so the UI is
   exercisable with no hardware and the sim page and the real UI are one
   renderer; (b) `esp_http_server` with `/ws` state push + 1 Hz heartbeat and
   `/api/` REST, both on the §10.2a dispatcher; (c) the pages; (d) ring upload
   staged and validated on the HTTP task, applied by the modes task; (e)
   assets gzipped into LittleFS (13,650 bytes for the whole UI plus
   `ring.json`).  Captive-portal provisioning, MQTT and OTA remain Phase 4;
   the Update page is a stub.
3.5. **Presentation mode** — the fullscreen terminal page above, entirely
   browser-side.  Delivered with the seconds freeze (§7.3), the measured wear
   API (`GET /api/wear`) and the real glyph artwork (`web/glyphs.svg`, 37
   symbols on a shared 100×127 viewBox, injected once and referenced
   same-document because external `<use>` references are unsupported in
   WebKit).
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
  **CLOSED 2026-08-23** — board V1.2, chip revision **v1.2**, production
  silicon. §2.0 has the full identification. The XIAO fallback is not needed;
  it stays the spare / project-#2 board.
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
- 2026-08-21 — **Phase 2 delivered** (ring, frame, modes, time, simulator).
  Decisions and facts recorded on the way:
  - **Seqlock precondition (Nico's gate) failed and was fixed first:** two
    consumers read pairs of relaxed atomics as if consistent (`revs` then
    `hall_to_hall` in the CLI; `state` then `hall_valid` in `go()`).  The
    control tick now brackets its publication with `AxisCtl::seq`;
    `axis_read_published()` is the only sanctioned multi-field read.
    MOTION_SYNC boundary (3) rewritten accordingly.
  - **Ring is runtime data** (§4 executed): `tools/ringgen.py` emits both
    `data/ring.json` and the compiled fallback header (`gen_ring_table.py`
    stays as a shim).  Role lookups derive from slot data, so a reordered or
    per-column ring keeps resolving; every parse failure falls back to the
    compiled table.  New managed component `joltwallet/littlefs` mounts the
    `storage` partition; the build never invokes the host-side image packer —
    `ring.json` arrives via the Phase 3 upload, fallback covers boot.
  - **Owned POSIX-TZ engine** (`timesvc/tz.cpp`): newlib/glibc/MinGW-UCRT
    disagree on M-rule TZ strings (UCRT cannot parse them), and DST edges
    must be host-tested to the second.  A dst name without explicit rules is
    rejected rather than guessed.
  - **Frame layer** (§6): pure scheduler over a MotionPort; land-on-tick
    starts the frame early by the longest column's modeled duration (pinned
    against the simulated controller); convergence in tick() resumes the
    frame after an automatic re-home — the §5.4 obligation now has an owner
    and a test.
  - **Countdown** (§7.3) runs on the deadline exactly as specced: cues derive
    from `target`, resume never replays past cues, the phase re-derives from
    the deadline at boot, one NVS write per set.  **`countdown.reveal`
    remains unset → the reveal frame is all blanks** until Nico picks the
    five glyphs (flagged, not resolved).
  - **MMM:S0 floor semantics noted:** 108:00 is the idle face; a running
    countdown shows it only for the start instant before rolling to 107:50 —
    that is `floor(remaining/10)*10`, verbatim from §7.3.
  - **WiFi is Phase 4:** until credentials exist SNTP never syncs, so a real
    boot shows blank then the centre-column WiFi glyph after 15 s.  That is
    §7.1 behaviour, not a defect.
  - **Simulator** (§14): rather than re-implementing mode logic in JS, a host
    tool (`gen_traces`) runs the real ModeManager/FrameScheduler and records
    go/spin/cue/mode events; `web/sim/index.html` replays them through a
    MockSocket speaking the intended Phase 3 `/ws` shape.  CI diffs the
    committed traces against a fresh run, so they cannot go stale silently.
- 2026-08-21 — **Adversarial review of Phase 2 (73 agents, refuter-verified);
  29 findings confirmed, collapsed into six root causes, all fixed or
  contracted:**
  - **Critical, fixed:** a finished countdown was never cleared from NVS and
    resume re-armed the choreography — any later power blip replayed the
    system-failure alarm and the 6 s spin, days after the run.  Now: leaving
    countdown mode after zero persists Idle (the reveal holds only while the
    mode does, per §7.3); any resume or set_target with the deadline in the
    past wakes **silently into the reveal** — cues and the spin belong to the
    real zero moment and never replay.  Pinned by tests.
  - **Major, fixed — pre-sync clock:** deadline commands (`execute`/`start`/
    `set_target`) are rejected until SNTP has synced ("time not synced"), and
    a persisted countdown's resume is **deferred** until validity arrives —
    comparing a 2026 deadline against the 1970 boot clock mis-derived
    everything.  An SNTP **time step** mid-run is now detected (any tick
    delta a 20 Hz cadence cannot produce): the message dwell keeps its
    remaining time, the clock re-renders, land-on-tick boundaries re-derive;
    the countdown needs nothing — its target is absolute.
  - **Major, fixed — concurrency:** ModeManager state was mutated from the
    CLI task while the 20 Hz modes task ticked.  Every public entry point now
    serializes on an internal std::mutex (FreeRTOS mutex with priority
    inheritance on target).  `ring_store::reload()` stays boot-or-modes-task
    only — contract in the header; the Phase 3 upload handler must dispatch
    it through the command path.
  - **Major, fixed — ring size:** `ring.json` with any slot count other than
    50 is now rejected (the drum is physical and `T(i)` is compiled for it);
    per-column overrides use the spec's `columns[i].ring` key (`slots`
    accepted as an alias).  The CLI's `go`/`ring` commands now resolve through
    the runtime table per column, not the compiled constants.
  - **Fixed — scheduler:** a spinning (open-loop) column's stale index no
    longer poisons `lead_ms` — an unknowable position budgets the full wrap.
    `mode.set message` with no live message is rejected instead of showing
    nothing; `display.frame` no longer silently eats a scheduled countdown
    boundary; `clock.land_on_tick` renders the current minute on entry.
  - **Minors, fixed:** TZ rules-without-dst-name rejected (was a silent
    multi-hour error); tzdb rule times to ±167 h accepted (Jerusalem/Gaza
    strings); the backslash-u0000 escape rejected in JSON (would smuggle a
    NUL into C strings); fall-back overlap comment corrected for negative-DST
    zones; the host FakePort now mirrors real open-loop index semantics and a
    non-instant-move test exercises the Moving paths; the simulator records
    the initial mode event and resets its mode label.
- 2026-08-22 — **Ring v3: descending, and two rings instead of one.**  The
  mechanical chat confirmed the redesign and generated
  `docs/ref/manifest_cols1234.json` (ring A, cols 1–4) and
  `docs/ref/manifest_col5.json` (ring B, col 5); the single
  `manifest.json` is retired.  Substance:
  - **All rings descend.**  A countdown decrement is now **1 flip on every
    column**, the show's single-flap tick.  Clock increments become the
    expensive direction (49 flips on ring A, 24 on ring B) — accepted, and
    managed by the new `clock.granularity_min`.  Reversing cols 1–4 is a
    **fidelity choice, not a timing requirement** (the mechanical chat's own
    correction): cols 1–4 would meet every deadline either way; they descend so
    the whole display ticks the same direction as the prop.  Column 5 is the
    one where descending is load-bearing.
  - **Column 5 gets two digit blocks** 25 slots apart, so its 0→9 wrap costs
    16 flips instead of 41.  That is what makes live seconds affordable, so
    **`countdown.seconds_mode` defaults to `seconds` (MMM:SS)** — reversing the
    earlier "MMM:S0, decided, do not revisit".  The reason for that decision
    was that a ones-of-seconds decrement cost 49 flips; it now costs 1.  MMM:S0
    survives as the low-wear `tens` option.
  - **A digit no longer has one slot.**  Lookups take the column's current slot
    and return the nearest match forward, so column 5's physical position is
    not predictable from the displayed digit.  Recorded in `docs/BRINGUP.md` so
    the calibration walk does not read it as a fault.
  - **`clock.granularity_min` defaults to 15.**  Measured from the manifests by
    walking a full day: 39,500 flips/day at 1 minute against 5,900 at 15.  Two
    corrections to the figures that prompted this: the ~43,000 col-5 estimate is
    actually **32,400** for col 5 alone (39,500 across all four moving columns),
    and **~2,300 is the total at 30-minute granularity**, not 15.  Also worth
    recording: at 15 minutes **column 4 is the wear bottleneck, not column 5**
    (3,600 against 1,200), because column 4 pays ring A's 49-flip increment
    while column 5 has the cheap 24-flip double-block one.
  - Correction to the stated flip costs: **col 4's minute rollover is 45 flips,
    not 49** (tens-of-seconds 0→5 on ring A).  49 is ring A's *increment* cost.
    Either number fits the 10-second budget — 45 flips is ~3.0 s at 15 flaps/s.
  - **Role coverage is now asserted at ring-load time** and the load is rejected
    on failure, so a table that cannot render something its column will be asked
    for fails loudly at boot rather than showing a blank column mid-show.
    Column 5 lacking AM/PM and wifi is legal and never asked.
  - The character-level helpers in `ring/ring.h` were **deleted**: they assumed
    one ascending digit block.  `ring.h` is slot arithmetic only, and
    `ring_forward_distance` remains direction-agnostic and never negative —
    which is the standing guarantee that no move can produce a reverse DIR.
  - `docs/ref/FIRMWARE_HANDOFF.md` §2 still described the ascending ring; its
    ring section is corrected in place and marked superseded by this spec.
- 2026-08-22 — **Column 4 is the clock's wear bottleneck, deliberately.**  At
  the default 15-minute granularity column 4 costs 3,600 flips/day against
  column 5's 1,200, because column 4 pays ring A's 49-flip increment while
  column 5 has the cheap 24-flip double-block one.  Giving ring A a second
  digit block would fix it and would mean another flap reprint; the clock's
  cost is already accepted, so this asymmetry stands as known and intentional.
  Recorded so it is not rediscovered as a defect.

- 2026-08-22 — **Phase 3 delivered** (web UI, WiFi STA, mDNS, `/ws` + `/api`,
  ring upload, gzipped assets).  Decisions and facts recorded on the way:
  - **The web API is a pure component** (`components/webapi/`): the state
    payload, the §10.2a dispatcher and the upload validator have no IDF
    includes, so the host dev server, the host tests and `esp_http_server` run
    the *same* code and a UI bug is reproducible on the desktop.
  - **A host dev server** (`tools/devserver/`) serves `web/` and speaks the
    real `/ws` against the real ModeManager over five `sim::SimAxis` columns —
    the Phase 1.5 simulator, so real homing, real edge verification and real
    forward-only motion.  It found two things immediately: the per-axis mailbox
    is single-slot and replace-on-write, so boot must home **before** starting
    the modes or the first frame silently supersedes the home request; and the
    frame scheduler issues a column's target twice in one tick (the frame, then
    the convergence pass, before the axis has reported Moving).  The duplicate
    is harmless — same target, replace-on-write — but `web/flap.js` ignores a
    repeat rather than restarting the animation.  Recorded rather than
    redesigned; it is on the list for the Phase 3 review.
  - **`countdown.reveal` is set by NAME and validated against that column's own
    ring.**  A numeric entry is now *refused* rather than read as "clear this
    column": the same index is a different character on column 5, so a number
    there has no safe reading.  The picker offers only what the column carries
    (36 glyphs on ring A, 29 on ring B).
  - **The drums' colour schemes travel with the ring** to the browser, so the
    mirror matches the wall (columns 4–5 are the inverted drums).  Presentation
    only; firmware behaviour never reads it.  `ringgen.py` emits it into the
    compiled fallback too, so a board with no `ring.json` still renders right.
  - **Ring upload:** the HTTP task validates entirely into a staging table and
    the **modes task** performs the swap, then `ring_store::write_accepted()`
    persists via temp-file-and-rename.  A malformed, truncated, oversized or
    role-incomplete upload leaves the running table untouched and never reaches
    the filesystem.  `test_api` hammers it with all of those plus every 97-byte
    prefix of a good document.
  - **The HTTP task never reaches mode state unlocked**, asserted at runtime:
    ModeManager bumps a witness counter inside its lock on every public entry
    point, and a three-thread test (modes task + two HTTP tasks) asserts it
    never exceeds 1.
  - **`motion.spin` added** to the command set — the Calibrate page's test
    spin (§10.2), through the one dispatcher like everything else.
  - **State push is rate-limited** to 5 Hz plus the 1 Hz heartbeat: the
    `go`/`spin`/`cue` events already carry the animation, and a 1.5 KB document
    at the tick rate is bandwidth the radio has better uses for.
  - **Assets ship gzipped only** (13,650 bytes for the UI plus `ring.json`,
    against a 2048 KB partition); `tools/webpack.py` fails the build if the
    payload passes its budget, so the UI cannot quietly eat the room audio
    needs.
  - **WiFi credentials come from the console** (`wifi <ssid> <pass>`) this
    phase.  No credentials is a supported state, not an error: the display is a
    standalone clock and shows the WiFi glyph after the grace period (§7.1,
    §10.0).

- 2026-08-23 — **The countdown seconds freeze** (Nico, from clicking through the
  dev server; a spec omission rather than a code defect).  The seconds columns
  now hold `00` until the run has `countdown.seconds_live_s` left — default
  **240**, the same instant as the 4-minute cue — and only then change
  resolution.  `countdown.seconds_mode` is redefined to say what happens
  *inside* that window: **minutes** (never), **tens** (MMM:S0), **seconds**
  (MMM:SS, default, show-accurate).
  - **The mode selects only the display step.**  60 s in the quiet phase, then
    10 or 1.  `render_countdown` lost its mode parameter and simply draws the
    value it is given; a step of 60 puts zeros in both seconds columns by
    construction, so "no column moves in the quiet phase except minutes" is a
    property of the arithmetic, not a special case that could rot.  The
    boundary is an ordinary step too — `countdown_next_shown_s(300) = 240`,
    `(240) = 239` — so land-on-tick crosses it with no extra logic.
  - **Measured, not estimated** (`test/host/test_wear.cpp`, printed and
    asserted).  Nico's estimate had column 5 at 600 and column 4 at 200 for the
    live window: **both exactly right**.  Two corrections: column 3 is **508,
    not ~104** — the ones-of-minutes digit pays ring A's 41-flip 0→9 wrap on
    each of the ten borrows, so the run totals **1,359, not ~915**.  And the
    pre-freeze baseline quoted as ~37,000 is neither of the two real ones:
    MMM:S0 throughout is **5,959** and MMM:SS throughout is **22,159** (the
    latter matching this spec's own earlier "~22,200, 16,200 on column 5").
    The freeze is still the right call by a wide margin: **22,159 → 1,359**,
    16×, and the show-accurate mode is now cheaper than the old low-wear one.
  - The 108:00 face rolls off at once, as the floor semantics have always said
    — a running countdown shows **107:00** half a second in.  Previously it
    showed 107:59; the visible change is the last two columns reading 00.

- 2026-08-23 — **Wear is computed, not tabulated.**  The Settings figure came
  from a five-entry JavaScript lookup and only moved for those five values.
  `components/modes/wear.cpp` now walks a whole day, and a whole run, through
  the REAL renderers against the loaded ring, and `GET /api/wear` serves every
  valid granularity and all three seconds modes.  Exact for any input, follows
  a ring upload, and cannot drift from the renderer.
  - **`clock.granularity_min` is constrained to divisors of 60** (Nico): a
    non-divisor does not tile the hour — at 7 the flooring steps 56 → 0 across
    the boundary.  Rejected at the API; the UI offers only the twelve.
  - It surfaced a fact worth having: **cost is not monotonic in granularity**.
    12 minutes (8,350/day) is worse than 10 (7,150) *and* 15 (5,950), because
    10 keeps the ones-of-minutes digit on 0 while 12 walks it through 0,2,4,6,8
    every hour.  An interpolated table would have hidden that.
  - The old §7.1 table was also 50 flips light in every row: it omitted column
    1's two AM/PM changes.  Corrected, with all twelve rows.
  - Cost on the device is small because the walk iterates the *floored* values
    rather than all 1440 minutes: ~4,700 renders for the entire document, on
    the HTTP task at priority 3.

- 2026-08-23 — **Real glyph artwork.**  `web/glyphs.svg` (Nico-supplied): 37
  `<symbol>`s — 36 glyphs plus wifi, `?` among them as `qmark` — on a shared
  `0 0 100 127` viewBox, ids matching the manifest names exactly, verified
  against the compiled ring tables with no gaps either way.
  - **Injected once and referenced same-document.**  `<use href="ext.svg#id">`
    is unsupported in WebKit entirely and fails over `file://`, so an external
    reference would have worked on desktop Chrome and rendered blank on iOS.
    `flap.js` fetches the sheet, injects it hidden, and **namespaces the ids**
    on the way in (`swan-glyph-…`): the file keeps the bare manifest names as
    specified, but names like `sun`, `hand`, `gate` and `wave` would otherwise
    join the page's single id space and could silently collide with a page
    element.
  - Not stretched: the 100:127 box is the flap card's ratio and each glyph is
    pre-scaled inside it, so `preserveAspectRatio` is left at the default and
    relative sizes match the printed cards.  Colour is `fill: currentColor`
    driven by the per-column scheme, so glyphs are red on black for the minutes
    group and black on white/red for the seconds group.
  - Implementation note: a percentage **height** on the SVG as a flex item did
    not resolve (it collapsed to 8 px); absolute positioning with `inset: 7%`
    resolves against the card every time.
  - Digits, AM/PM and blank remain text placeholders, and a missing symbol id
    falls back to the manifest name — which is also what makes a table/drum
    mismatch obvious on the Calibrate walk.  Cost: **+20,747 bytes gzipped**.

- 2026-08-23 — **Phase 3.5, presentation mode** (`web/terminal.html`).  A
  fullscreen Swan terminal with the flap display docked small, an on-screen
  keypad, a CRT effect defaulting off, and WebAudio key clicks.  Recorded
  decisions:
  - **The curvature is a CSS approximation** — perspective tilt, rounded bezel,
    corner vignette — not a true lens warp.  A real `feDisplacementMap` needs
    an `feImage` source, is expensive to composite, and is unreliable in
    WebKit; this is a presentation flourish, not instrumentation.
  - **Preferences are `localStorage`, per browser, never NVS.**  Two people
    looking at the same display should not fight over each other's scanlines.
  - **The page is a candidate terminal prop** (§10.2) — a Pi in kiosk mode
    against `lost.local` — so it is laid out for a kiosk screen and scales
    down.  A prop built this way needs no MQTT and no second countdown
    implementation.
  - `web/bus.js` now carries the /ws transport for both pages; a second copy of
    the reconnect logic is a second place for it to be subtly wrong.

- 2026-08-23 — **Adversarial review of Phase 3** (80 agents; 7 finder lenses,
  every finding put to three independent refuters, 22 confirmed / 2 refuted),
  scoped as Nico asked to the upload path, task boundaries and the frame
  double-issue window.  Collapsed to 15 root causes; all fixed.  The four that
  mattered most:
  - **CRITICAL — `/ws` never registered a single client on target.**
    `esp_http_server` answers the WebSocket handshake itself and deliberately
    does *not* call the URI handler for it (`httpd_uri.c`: "If the request is
    websocket handshake, then do not call the uri->handler").  Registration
    lived in that handler, so `g_ws_fds` stayed empty for ever: no state
    document, no 1 Hz heartbeat, no `go`/`spin`/`cue`, a blank mirror — while
    *commands still worked*, because a reply goes back through the session's
    own handler, which is what would have made this maddening to debug on the
    bench.  Now registered from `ws_post_handshake_cb`
    (`CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT`), and a plain `GET /ws` with
    no Upgrade header is refused instead of being registered as a socket that
    then receives raw frames.  **The host dev server could never have caught
    this** — it has its own socket layer.  Verified against the IDF source,
    not taken on the reviewer's word.
  - **CRITICAL — the ring swap was a use-after-free.**  `apply_pending()` does
    `live_ = *staged_`, dropping the last `shared_ptr` to the outgoing tables
    and freeing 100 `std::string`s, while the httpd task held
    `const RingTable&` across a whole response — and the shipped UI fires
    `GET /api/ring` and `/api/wear` the instant an upload returns `ok`.  The
    modes task is priority 5 against httpd's 3 on one core, so it preempts
    unconditionally.  Fixed by pinning: `api::RingSource::snapshot()` returns a
    cheap copy taken under the swap lock, whose `shared_ptr`s keep the old
    tables alive for as long as the reader holds it.  Every reader outside the
    modes task — four httpd routes, three CLI commands — now goes through it,
    and the swap itself goes through `ModeManager::cmd_ring_swap` so it holds
    the same lock every command takes.  `ring_store.h`'s contract only ever
    constrained *writers*; it now states the reader rule too.  The dev server's
    coarse `dev_mu` had been masking it.
  - **CRITICAL — a Running countdown was never ticked from another mode.**
    `enter_mode` deliberately keeps a live deadline across a mode switch, but
    only the countdown *mode* advanced it.  Switch to the clock mid-run and the
    cues never fired, zero never happened — and then everything fired at once,
    alarm spin included, whenever countdown mode was next entered.  A run
    started before bed detonated the next time anyone opened the Modes page.
    Cues and phase are now unconditional (`tick_countdown_offscreen`); only the
    *rendering* is mode-gated, and an off-screen zero lands silently in Reveal
    rather than seizing five columns it does not own.
  - **CRITICAL — the JSON parser had no node budget.**  Input length was capped
    at 256 KB and depth at 16, but each `Value` costs ~64 bytes on RV32 against
    a 2-byte minimum token — ~32×, into one contiguous vector.  With exceptions
    off and no PSRAM the allocation failure is `abort()`, so a ~5 KB
    `{"slots":[0,0,0,…]}` — *smaller than the legitimate ring.json*, and
    reachable by picking the wrong file in Settings — was a remote reboot loop.
    `MAX_NODES = 8000` now, and `RING_UPLOAD_MAX` down from 64 KB to 24 KB
    against a 9.4 KB document.

  The rest, briefly: `read_body` retried a recv timeout for ever, so one
  stalled POST wedged the single httpd task permanently; a zero-length WS frame
  desynchronised the stream; a queued async send could land on a recycled fd
  belonging to a plain HTTP connection; `send_wait_timeout` cut to 1 s so a
  sleeping phone cannot hold the task for five; applying a ring never forced a
  re-render, so the drums stayed on slots from the old table — indefinitely in
  message, preset, reveal or countdown-idle; `write_accepted` removed
  `ring.json` before renaming over it, on a false premise (LittleFS `rename`
  *does* replace atomically), so a brownout in that window lost the table
  entirely; `seconds_live_s` that was not a whole minute made the countdown
  count **up** across the freeze boundary, paying a 45-flip borrow and a
  16-flip wrap in the wrong direction — rejected at the API and floored
  defensively in the core; `countdown.set_target` accepted an unbounded epoch,
  so a millisecond timestamp parked the display on 000:00 while reporting
  `running` — bounded to 24 h; `Context::tz` was a `std::string` written on the
  httpd task and read at 20 Hz on the modes task, now owned by ModeManager and
  read under its lock; and `ReqKind::Stop` left `index` stale after an
  open-loop move, so the axis advertised a face it was not showing and
  convergence never corrected it.
  - **The frame double-issue window**, which Nico asked to have looked at
    specifically, turned out to be two real bugs rather than the harmless
    duplicate it was recorded as in the Phase 3 entry.  Within one modes tick
    the axis still reports its pre-command state, because the mailbox is
    drained by the 1 kHz control tick.  `issue()` therefore skipped a column
    whose new target equalled its *stale* index — leaving a superseded command
    to execute — and the convergence pass overwrote the alarm spin it had just
    started, deterministically whenever `zero_hold_s = 0`.  `FrameScheduler`
    now records what it commanded during the current tick; `docs/MOTION_SYNC.md`
    has the full contract.  `FakePort` settled moves instantly and so could not
    express the bug at all; it grows a `mailbox_lag` mode, and both cases are
    pinned by tests that were confirmed to fail without the fix.
  - Two findings were refuted and are recorded as *not* defects: a rehome
    posted from the CLI or `/api` cannot be destroyed by the frame layer (task
    priorities forbid the sequence), and `write_accepted`'s discarded error was
    real but is now logged loudly rather than silently dropped.
  - Method note: every fix that could be regression-tested was, and each new
    test was checked by reverting its fix and confirming it fails.  That caught
    two defects in my *own* first attempt at the scheduler fix.

- 2026-08-23 — **First flash on real hardware.**  Board arrived; §2.0 open risk
  closed (chip v1.2, production silicon, no revision abort).  What the board
  showed that nothing else could:
  - **`ring.json` was shipping gzipped and therefore never loading.**
    `tools/webpack.py` compressed every `.json`, but `ring_store.cpp` opens
    `/fs/ring.json` directly and knows nothing about gzip — so the image
    carried `ring.json.gz`, the boot log said *"no /fs/ring.json; compiled ring
    table active"*, and the compiled fallback silently covered for it.  Nothing
    downstream looked wrong, because the fallback is a correct table; the fluid
    runtime ring was simply dead.  Fixed: files the FIRMWARE reads are never
    compressed, only browser assets are.  Now logs *"ring table loaded from
    /fs/ring.json (50 slots)"*.  Payload 46,195 → 54,119 bytes gzipped.
    A host test cannot catch this — it is a property of the flashed image.
  - **Unwired homing fails exactly as designed.**  With no motors, drivers or
    Halls attached, all five columns run 1.2 revolutions at homing speed, time
    out, retry three times on the 250 ms stagger, then latch FAULT with
    "gave up after 3 re-homes; `home 0` clears it".  Position freezes at
    39,560 µsteps, velocity 0, and the console stays responsive throughout —
    it fails, it does not hang.  `home 0` clears that column's fault and leaves
    the other four latched, as documented.
  - **Pin map initialises clean.**  `pins` reports STEP 8/9/10/11/12, HALL
    0/1/4/5/23, EN 6, I2S 7/25/26, BUTTON 28, LED 27 — no STEP, EN or HALL on a
    strapping pin, only the high-Z I2S inputs, matching §2.2.  Halls read
    `raw=1 magnet=no` on all five, the correct unwired state for an
    active-low input with a pull-up.
  - Free heap after boot with WiFi initialised, httpd serving and LittleFS
    mounted: **141 KB**.  App 1,260,128 bytes, 52 % of the partition free.
    LittleFS 80/2048 KB used.
  - Bench note recorded for the tooling: pyserial asserts DTR and RTS on open,
    and on USB-Serial-JTAG those drive EN and the boot pin — so a naive
    open-per-command **resets the board between commands**.  Any scripted
    console interaction must clear both before `open()`.

- 2026-08-23 — **"Position unknown" must not look like "showing blank"**
  (Nico, from the first LAN session).  With nothing wired, all five columns sat
  at `idx=-1` while hunting for a hall edge, and the mirror rendered them as
  blank cards — so a display that was actively searching looked like a display
  sitting idle, and nothing on any page said otherwise.  The mechanism was
  `FlapDisplay.setAll()` skipping negative indices, which left each card on
  whatever it was painted last: slot 0, blank.
  - `paintUnknown()` now renders unknown distinctly — hatched, dimmed, dashed
    border, `title="position unknown"` — and `HOMING`/`UNHOMED` adds a slow
    pulse so hunting reads differently from given-up.  An open-loop spin ends
    in the same state, because that is exactly what it leaves behind.
  - The state payload gained **`settled`** (Idle *and* index >= 0) and
    **`retry`** (the automatic re-home attempt, published through the seqlock
    alongside everything else the control tick owns).
  - A **persistent banner** sits outside `<main>` on the control panel and as a
    header chip on the presentation terminal, so it is visible from every page
    rather than only in Diagnostics.  It names the columns and the attempt.
  - Timing worth knowing, measured on the board: a homing pass is **~7.5 s**
    (1.2 revolutions at 8 flaps/s), a column tries **three** times, so there is
    a **~30 s window from boot** in which the display looks idle and is
    actually searching.  That window is now legible.
  - Correction to the previous entry: I reported "all five settle in FAULT" as
    the outcome.  That is the *end* state; the state that matters, and the one
    that was invisible, is the half-minute of hunting before it.

- 2026-08-23 — **The JSON DOM is too heavy for untrusted input on this
  device**, found by flooding the real board rather than by reasoning.  The
  phase 3 review's node cap of 8,000 was fifteen times too generous:
  - Measured: `data/ring.json` is 9,361 bytes and **535 nodes**.  A 500-node
    flood parsed in 0.1 s; **2,000 nodes panicked the board every time**
    (`reset=panic`), and the arithmetic is exact — one array growing to a
    capacity of 1024 needs 1024 x 64 = 65,536 bytes while still holding the
    512 x 64 = 32,768 it is copying from, and **98,304 was precisely the
    largest free block** the board reported.
  - So the hazard is **one huge container**, not the total node count.  Two
    limits now: `MAX_NODES_UNTRUSTED = 700` overall, and **256 elements per
    array or object** — five times what any legitimate document needs, and it
    stops a flood at byte 523 in 0.00 s, before any memory pressure.
  - The upload handler also refuses when the largest free block cannot hold the
    parser's worst case.  The first attempt at that guard derived the
    requirement from the *body size* and let a 4 KB flood through to panic the
    board; it is derived from the parser's own caps now.
  - `json::parse` gained an explicit `max_nodes`, because the default is a
    policy for untrusted input on a 128 KB heap — not a property of the format.
    A host test checking our own `/api/ring` (~1,850 nodes) states its own.
  - **The real fix is a streaming parse for the upload path**, which is Phase 4
    work.  The headroom with a DOM is genuinely thin and this is recorded so it
    is not rediscovered.

- 2026-08-23 — **Hardware verification of the Phase 3 review's finding 1, and
  of the upload path against real LittleFS.**
  - `/ws` on real silicon: handshake 101, the client **registers** (it receives
    anything at all, which requires the post-handshake callback to have run),
    state pushes flow and the clock advances.  The fix holds on the hardware
    the dev server structurally could not test.
  - State pushes were pinned at the 5 Hz cap forever, because free heap and
    uptime jitter every tick and the change comparison covered them.  It covers
    the **display state only** now — `"mode"` up to `"sys"` — and an idle
    display pushes at exactly **1 Hz**, measured.
  - Ring upload against real LittleFS: malformed, truncated, 49-slot,
    role-incomplete and node-flood documents are all rejected in 0.00 s with
    accurate reasons, the running table untouched and no reboot.  A valid
    modified table uploads, applies, **persists across a reboot**, and — the
    part that matters — **drives resolution**: a token that exists only in the
    uploaded table resolves, and one that exists only in the compiled fallback
    is rejected.  The runtime ring is live, not decorative.


- 2026-08-23 — **Per-column state, fault escalation, maintenance mode, and
  simulated axes on the target** (Nico: "not a dev scaffold - this is how the
  display should behave permanently, including repairs years from now").  New
  spec §5.8, §5.9, §5.10; §7.4 rewritten so the `[Q5]` FAULT display policy is
  stated as executed rather than as a one-line answer.  What was decided:
  - **Three causes, two responses.**  Nico asked for two causes wanting
    opposite responses.  There are two responses, and **three** causes, because
    a *slip* - the edge arrived in the wrong place - is retryable for a
    different reason than a dead sensor is: the drum turned, it just lost
    registration, and a re-home is the recovery.  Classifying slip as a jam
    regressed Phase 1.5's slip-recovery assertions (`test_axis_sim`, which
    requires `rehomes >= 1` and `gave_up == 0` after a +200 µstep slip), which
    is how the distinction was found rather than argued.
  - **`edge_overdue` widened from rev + 1 flap to rev + half a revolution.**
    At the tight threshold a slip of just over one flap and a completely
    stopped drum are the *same observation*, so a jam could not be distinguished
    from a slip at all.  Widening makes a slip resolve as a late edge (Slip,
    retried) and only a genuine absence trip the Jam path.  Cost: a real jam is
    noticed up to ~3.1 s later at 15 flaps/s — against the alternative of
    spending a full 7.5 s homing pass grinding into it, which is the thing this
    whole change exists to prevent.
  - **EN is ganged; the spec now says so where it matters** (§2.2, §5.8,
    BRINGUP).  One GPIO for five drivers and exactly one spare non-strapping
    GPIO, so `park_column` and `stop_column` stop *stepping* while the coils
    still hold standstill current.  `drop_enable` is the only true de-energize
    and necessarily takes the whole display.  That is why "several columns at
    once" and "a fault during the alarm spin" both drop EN rather than trying
    to isolate a column the hardware cannot isolate.
  - **Frame holes: Nico's preference adopted, with one refinement.**  Keep the
    mode running, leave the hole, banner names which columns are out and why.
    The refinement: a column being disabled is **parked on blank first** while
    it is still homed and idle, so the hole reads as a blank flap rather than a
    stale digit somebody would read as part of the time.  A clock reading
    `09:4[7]` with a dead 7 is worse than one reading `09:4_`.
  - **`disabled` is never inferred.**  No fault, timeout or escalation writes
    it; a fault stays a fault and stays visible until cleared.  Asserted in
    `test_fault_policy` by the shape of the policy layer: escalation's
    vocabulary is entirely about stepping, and nothing in it returns a
    `ColumnMode`.
  - **Maintenance survives a reboot on purpose**, and a boot in maintenance
    does not home and leaves EN released.  Cues are held too — a
    system-failure alarm going off while someone has their hands in the
    mechanism is exactly wrong.  A running countdown keeps its deadline (it is
    absolute) but is not driven.
  - **Simulated axes substitute exactly one line in the ISR** — the Hall source
    — so everything above the axis layer is the same code on the same path.
    The drum model is division-free (a 64-bit divide in the IRAM ISR would call
    flash-resident `__udivdi3` and stall behind an NVS write) and uses the real
    272000/33 as an integer DDA, so the 0.42 µstep/rev residue accumulates as
    the real drum's does and a homing pass takes the real ~7.5 s.
  - **Never the default, enforced three ways:** `static_assert` on
    `ColumnConfig`'s defaults, a host test that reads the same assertion aloud,
    and a configure-time `FATAL_ERROR` if `SWAN_RELEASE=1` is built with
    `SWAN_SIM_AXES` still ON.  CI builds the release configuration to prove the
    gate fires.
  - Recorded in BRINGUP: the firmware detects a **stall** (the drum stopping
    while the motor steps) but **cannot** detect cards fluttering or failing to
    seat — drum position stays perfectly correct while the display looks wrong.
    Speed tuning is therefore **watched**, not just logged.
