# Bring-up — checklists and results

Results go in this file as they come in. If a bench result contradicts the spec,
the spec is wrong: fix `docs/FIRMWARE_SPEC.md` and append to its §17 decision log
(CLAUDE.md).

## Verification status

Phases 1-3.5 build and their host tests pass.  **The board arrived 2026-08-23
and the firmware is flashed and booting**; nothing is wired to it yet, so the
motion steps below still need the mechanics.  Steps 0-1 are done, and the
unwired half of the fault path is verified.

| gate | status |
|---|---|
| `.\build.ps1 set-target esp32c5` + `.\build.ps1` | **passes** — ESP-IDF v5.5.5, DevKitC-1 1,251 KB app (52% of the partition free), zero warnings |
| `.\build.ps1 -DSWAN_BOARD=xiao build` | **passes** — 1,232 KB. Exercises the alternate pin map and its strapping-pin `static_assert`s |
| LittleFS payload | **46,195 bytes** (UI, presentation terminal, glyph sheet + `ring.json`, gzipped) of a 2048 KB partition |
| host tests (`.\test-host.ps1`) | **9/9 pass** — rings, motion math, simulated axis, ring.json, TZ/DST, frame, modes, wear, web API |
| `git diff` empty after `tools/ringgen.py` | **clean** — regeneration is byte-identical |
| motion cross-task handoff explicit | **done** — spinlock + request mailbox + relaxed atomics + the `AxisCtl::seq` seqlock (`docs/MOTION_SYNC.md`) |
| chip revision ≥ v1.0 | **v1.2** — production silicon, verified by esptool and the bootloader |
| first flash + boot | **done** — CLI up on COM3, ring.json loads from LittleFS, no revision abort |
| unwired homing fails cleanly | **done** — all five latch FAULT after 3 re-homes; no hang |
| pin map, no strapping conflict | **done** — `pins` matches §2.2 |
| every bench step needing mechanics | **not run** — needs motors, drivers and Halls |

What passing host tests do and do not prove: `T(i)` rounding, the 8242/8243
revolution alternation, forward-distance costs over all 2500 index pairs, edge
classification thresholds, that the ramp plus DDA lands on the target exactly,
clock rendering across DST edges, the countdown schedule from a deadline, and
every web command and the upload validator round-tripped through the JSON
layer are all verified in software. Nothing about the Hall wiring, the gear
ratio, driver behaviour, flap mechanics, radio range behind the aluminium
faceplate, or mDNS on a real LAN is verified by them.

## Phase 1 bench checklist (spec §14.1)

Run with one module on the bench, drivers powered from 12 V, logic from the buck.

### 0. First flash — **DONE 2026-08-23**

Result, for the record:

```
Chip is ESP32-C5 (revision v1.2)      BASE MAC: 10:bd:a3:dd:a8:e8
ESP-ROM:esp32c5-eco3-20250704         rst:0x15 (USB_UART_HPSYS)
I (22) boot: chip revision: v1.2      efuse block revision: v0.3
I (257) efuse_init: Min chip rev: v1.0   Max chip rev: v1.99   Chip rev: v1.2
I (280) ring: littlefs mounted: 80/2048 KB used
I (293) ring: ring table loaded from /fs/ring.json (50 slots)
I (419) httpd: serving on port 80
swan>
```

No revision warning and no abort — v1.2 is inside the image's window.  Free
heap after boot with WiFi initialised and httpd serving: **141 KB**.

Unwired behaviour, which is what this checklist step is really for.  **Read
the timeline, not just the end state:** every column runs 1.2 revolutions at
homing speed — **~7.5 s** — times out, and retries **three** times on the
250 ms stagger.  So for roughly **30 seconds from boot the columns are hunting**
with `idx=-1`, and only then do they latch FAULT and stop, at 39,560 µsteps
with velocity 0.  The console is responsive throughout and `home <col>` clears
that column, leaving the others latched.

That hunting window used to be invisible: the mirror painted an unknown index
as a blank card, so a searching display looked like an idle one.  The web UI
now renders unknown distinctly (hatched, pulsing while hunting) and carries a
persistent banner naming the columns and the re-home attempt — see step 16b.

### 0b. First flash — the original checklist

```bash
idf.py set-target esp32c5
idf.py build
idf.py -p <PORT> flash monitor
```

- [ ] Bootloader prints `chip revision: v1.0` or later. **v0.1 goes back** —
      current ESP-IDF does not support it. Record the revision in `README.md`.
- [ ] Record the exact IDF version (`idf.py --version`) in `README.md`.
- [ ] Boot log shows the board name and the resolved drive constants.

Expected on a bench with no motors attached: all five columns go UNHOMED →
HOMING → FAULT after ~1.2 revolutions of stepping, since no Hall edge ever
arrives. That is the correct behaviour, not a bug.

### 1. `pins`

- [ ] Map matches the table in `README.md`.
- [ ] `usteps/flap` prints `5440/33 = 164.8485`.

### 2. `hall` — polarity and active level

Wave the magnet past the sensor.

- [ ] `raw` goes 1 → 0 as the magnet approaches (A3144 is open-collector, pulls
      LOW on assert).
- [ ] `magnet` reads `YES` while the magnet is present.

If `magnet` is inverted, the config default is wrong, not the code:
set `motion.hall_active_low` false. If `raw` never moves, it is the magnet
**face** (A3144 is unipolar — BOM gotcha #2) or the 5 V supply, not firmware.

- Result: hall_active_low = ______

### 3. `step 0 200` — direction

- [ ] Drum turns so the **flap fronts fall forward** — that is the one legal
      direction, and on the v3 descending rings it is the direction in which
      the displayed digit DECREMENTS.

If not, move that driver's DIR tie to the other rail. DIR is tied per driver, so
this is a per-column wiring fix; there is no firmware setting for it and coil
order on the JSTs does not need to match.

- Result: DIR rail = ______

### 4. `home 0`, then `revs 0 10` — the gear ratio question

This is the step that settles the 85T/33T vs 68T/26T conflict between
`FIRMWARE_HANDOFF.md` §1 and `MECHANICAL_README.md:67`.

| hall_to_hall | means |
|---|---|
| ~8242–8243 | 85/33 at 1/16 — the spec is right, nothing to change |
| ~8369 | 68/26 drum — change the four constants at the top of `components/ring/include/ring/geometry.h`, rebuild, and correct spec §3 |
| anything else | microstep setting is not 1/16, or the motor is not 200 steps/rev |

An 8369 drum shows up as a Major resync warning every revolution rather than a
fault, so the measurement still completes.

- Result: hall_to_hall = ______  → gearing = ______

### 5. `spin 0 <flaps_s> 10` — 10 → 25 flaps/s with flaps installed

- [ ] Note the rate at which flaps stop clearing cleanly. That number sets
      `motion.flaps_s_alarm`.

Flap fall is gravity-limited around 20–25 flaps/s regardless of motor; the real
ceiling is measured here, not assumed.

> **WATCH THIS ONE, do not just read the log.**  The firmware detects a
> **stall** — the drum stopping while the motor keeps stepping — because that
> moves the Hall edge.  It **cannot** detect cards fluttering, bouncing, or
> failing to seat against the bezel lip: the drum position stays perfectly
> correct and every counter stays clean while the display looks wrong.  There
> is no sensor that sees a card.  So this step is the one place where your eyes
> are the instrument, and `flaps_s_alarm` is set by watching, not by a number
> the console prints.

- Result: clean up to ______ flaps/s → flaps_s_alarm = ______

### 6. Edge repeatability — 20 revolutions

`revs 0 20`, read the reported spread.

- [ ] Set `motion.hall_tol` from it. The default 41 (¼ flap) is a placeholder.

- Result: spread = ______ → hall_tol = ______

### 7. `en 0` for 10 minutes with a loaded drum

- [ ] Does it creep? If it holds, `motion.en_idle_off` can default true (cooler
      motors). Until measured it stays false.

- Result: creep = ______ → en_idle_off = ______

### 8. Calibration

- [ ] `cal <col> <±n>` until the blank card hangs flat against the bezel lip and
      the next card is fully retained. `save`.
- [ ] `go <col> <i>` for every index 0..49 — confirm the mapping against the
      table `ring` prints.

`ring` lists the loaded table and says whether it came from `ring.json` or the
compiled fallback, and whether it reads as descending.

**Three part numbers — check you have the right drum on the right column.**
The v3 rings differ by column, and a swapped drum is diagnosed from this walk,
not from a fault:

| columns | ring | tell-tale during the walk |
|---|---|---|
| 1, 2, 3 | A, black cards, **0 straddle flaps** | slot 1 = wifi glyph, 38 = PM, 39 = AM, 40–49 = digits 9→0 |
| 4 | A, white/red cards, **straddles at flaps 1 and 37** | same ring order as 1–3; only the card colour differs |
| 5 | **B**, straddles at flaps 0, 14, 24, 39 | **digits appear twice** — 15–24 and 40–49, both 9→0; **no AM/PM, no wifi glyph** |

So: if column 5 shows AM, PM or the wifi glyph anywhere in the walk, it has a
cols-1–4 drum on it.  If columns 1–4 show digits twice, they have the column-5
drum.  If column 4's cards are black rather than white/red, it has a 1–3 drum —
the ring order will look right, which is exactly why the colour is the tell.

**Column 5's position is not predictable from its digit — this is not a
fault.** Because ring B carries each digit twice, the firmware picks whichever
slot is nearest going *forward*, so showing `7` may mean slot 17 or slot 42, and
the same displayed value can sit in either block at different times.  Expect
that during the walk and during a countdown; it is the mechanism that makes the
0→9 wrap cost 16 flips instead of 41 (spec §4).

Note: the calibration offset is normalised into [0, one revolution).  A negative
nudge is reported as a large positive number and costs most of a revolution to
become visible — that is the one-way ring, not a firmware bug.  Nudge forward.

- Result: cal[0..4] = ______ / ______ / ______ / ______ / ______

## Phase 3 bench checklist — network and web UI

Runs after the Phase 1 motion checks; nothing here touches the motion path.

### 9. Join a network

- [ ] `wifi <ssid> <password>` on the console.  Expect `saved; connecting` then
      a `wifi: connected, ip …` log line within a few seconds.
- [ ] `wifi status` — state, SSID, IP, RSSI, and the disconnect counter.
- [ ] **RSSI with the aluminium faceplate fitted** (spec §2.4 open risk: the
      DevKitC-1 has a PCB antenna and the faceplate is an RF shield).  Record
      both numbers; if the fitted value is bad, that is the u.FL/XIAO argument.
- Result: RSSI open ______ dBm · faceplate fitted ______ dBm

With no credentials the display is a standalone clock: SNTP never syncs and the
centre column shows the WiFi glyph after 15 s.  That is spec §7.1, not a fault.

### 10. Reach the UI

- [ ] `http://<ip>/` loads.
- [ ] `http://lost.local/` loads (mDNS; some Android browsers do not resolve
      `.local` — test from a laptop or an iPhone before calling mDNS broken).
- [ ] The five columns mirror the real drums, and columns 4–5 render in the
      inverted colour scheme.
- [ ] The connection chip reads **connected**; pull the AP and it goes red and
      reconnects on its own.
- [ ] Diagnostics shows `h2h` in the 8242–8243 band for every column.  A
      consistently different value means the gear teeth or the microstep
      setting differ from spec §3 — the drum wins, the spec gets corrected.

### 11. Control paths agree

Every control is the same command over three transports; they must behave
identically.

- [ ] Numbers via the UI's EXECUTE, wrong ones rejected with a toast.
- [ ] The same via the console: `countdown execute "4 8 15 16 23 42"`.
- [ ] Switch modes from the UI while a countdown runs; the console `stats`
      agrees with what the page shows.

### 12. Calibrate from the page

- [ ] The ±1 / ±10 nudges move the drum and the offset readout tracks.
- [ ] The index walk steps the column and the *expected* character is shown
      beside each stop — this is the table/drum mismatch check from step 8,
      with the answer printed next to the flap.
- [ ] The live speed sliders change the sound of a move immediately; SAVE is
      what survives a reboot.  Confirm both halves separately.

### 12b. Motion state is legible — **DONE 2026-08-23 (unwired)**

- [x] A column with an unknown index renders **differently from blank** —
      hatched and dimmed, `title="position unknown"`.
- [x] While hunting it pulses, and the banner reads
      `COLUMNS NOT SETTLED — col 1 re-homing 2/3 · …` with the ~7.5 s / three
      attempts note.
- [x] Once given up it reads `MOTION FAULT — col 1 FAULT (gave up after 3
      re-homes) · …` and points at REHOME.
- [x] Visible from every page, and as a header chip on the presentation
      terminal (a kiosk never opens Diagnostics).
- [ ] With drums attached: confirm a column that homes successfully drops the
      indicator promptly and the card stops being hatched.

### 13. Ring upload

- [x] **DONE 2026-08-23 against real LittleFS.**  Malformed, truncated (4,679 B),
      49-slot, role-incomplete and node-flood (4 KB and 18 KB) documents were
      each rejected in **0.00 s** with an accurate reason, the running table
      untouched and **no reboot**:
      `bad literal at byte 0` · `expected ',' or '}' at byte 4681` ·
      `array has too many elements at byte 523` ·
      `shared table has 49 slots; the drum has 50` ·
      `column 1 cannot render role 'AM'`.
- [x] A valid modified table uploaded, applied, and **survived a reboot** with
      `source: ring.json`.  It demonstrably **drives resolution**: a token
      present only in the uploaded table resolved, and one present only in the
      compiled fallback was rejected.
- [ ] With drums attached: confirm an upload actually re-renders the wall
      (`cmd_ring_swap` forces it) rather than leaving the columns on slots
      chosen from the old table.

### 14. Assets

- [ ] The UI loads with the browser cache disabled (gzipped assets served with
      `Content-Encoding: gzip`). Whole payload is ~46 KB; if a page is blank,
      check the LittleFS image was flashed — `idf.py flash` writes it.
- [ ] The glyph cards show **artwork**, not names. Names are the designed
      fallback, so nothing breaks — it just looks wrong; check `GET /glyphs.svg`.
- [ ] **Check it on an iPhone.** The sheet is injected and referenced
      same-document precisely because WebKit does not support external `<use>`;
      a regression there shows as blank cards on iOS only.

### 15. The countdown freeze (spec §7.3)

- [ ] Start a countdown. The display reads **MMM:00** and the last two columns
      do not move at all for the first hour and three quarters — only the
      minutes columns tick.
- [ ] At **004:00** the seconds wake: column 4 takes the 45-flip 0→5 borrow and
      column 5 the 16-flip 0→9 wrap, then 004:00 → 003:59 → 003:58. Watch that
      it *lands* on the boundary rather than starting there — the frame is
      issued ~3 s early on purpose.
- [ ] The 4-minute cue fires as the seconds wake. That coincidence is the point.
- [ ] `minutes` mode: cols 4–5 never move for the whole run.

### 16. Presentation terminal

- [ ] `http://lost.local/terminal.html` on the kiosk machine, fullscreen.
- [ ] The keypad enters the Numbers and EXECUTE is accepted; a wrong entry says
      so without touching the display.
- [ ] Key clicks sound; toggling them off silences them and survives a reload.
- [ ] The CRT toggle is off by default. Turn it on and **measure the frame rate
      on the actual kiosk hardware** — a Pi compositing a fullscreen scanline
      overlay is the case that matters, and the toggle exists because it may
      not be free.
- [ ] The readout counts down smoothly between state pushes (it derives from
      the deadline locally) and agrees with the drums.

### 17. Per-column mode and simulated axes (spec §5.9, §5.10) — **DONE 2026-08-23 (unwired)**

Run on the board, `12dbb0f`+. Results:

```
col                       -> all five `real`, maintenance off   (fresh NVS, correct)
(boot, nothing wired)     -> five columns hunt, then:
  col 4 FAULT: no hall edge in 1.2 revolutions
  col 4 gave up after 3 re-homes
  col 4 fault during a multi-column failure -> DROPPING EN FOR ALL FIVE
col                       -> all five FAULT (no_hall)
sim all                   -> five "col N is SIMULATED - no motor is being driven"
en 1; home all            -> all five homed, ~0.7 s apart on the 250 ms stagger
sim fault 0 slip 200
spin 0 15 12              -> col 0 FAULT: hall edge off by more than one flap (err=200)
                             col 0 re-home attempt 1/3
                             col 0 parked: slip. The other columns keep running.
                             col 0 homed at pos=63252            <- recovered
sim fault 1 miss 3
spin 1 15 15              -> col 1 FAULT: no hall edge for 1.5 revolutions - drum stopped?
                             col 1 latched FAULT without retrying
                             col 1 STOPPED - this looks like a JAM, not a sensor fault.
col                       -> col 1 FAULT (jam), the other four IDLE
col 3 disabled            -> "col 3 disabled; parking it on blank first"
maint on                  -> "maintenance ON; EN RELEASED (all five - it is ganged)"
save; reboot              -> boot log: 4 SIMULATED, 1 DISABLED, MAINTENANCE MODE,
                             no homing, drivers disabled, all five UNHOMED
maint off                 -> re-homes four; col 3 stays UNHOMED (disabled is never homed)
GET /api/state            -> motion{simulated:true, sim_columns:4, disabled_columns:1,
                                    maintenance:false, sim_available:true}
                             faces AM · blank · 8 · <hole> · 0   <- 08:00 with the hole
POST /api/cmd motion.column {column:3, mode:"sim"}   -> ok, hole closes, 08:30
POST /api/cmd motion.column {column:3, mode:"banana"} -> {"ok":false,
                             "err":"mode must be real|sim|disabled"}
```

The whole clock — real ModeManager, real FrameScheduler, real control core,
real edge verification — runs against modelled drums on real silicon with real
WiFi, LittleFS, NVS and heap. Free heap all-sim: **132,288 B**.

One log line was wrong and is fixed: a jam latches with **zero** retries but
the give-up message printed `REHOME_RETRIES`, so it claimed three re-homes
directly above the message saying it deliberately did not retry.

#### The original checklist

Runnable with **nothing wired**, which is the point: it is how phases 4 and 5
get exercised on real silicon, real WiFi, real LittleFS, real NVS and a real
128 KB heap while the mechanics are still weeks out.

- [ ] `col` — lists all five.  A fresh NVS must read `real` on every column and
      `maintenance off`.  If it does not, the defaults are broken; stop.
- [ ] `sim all` → all five simulated.  Console prints the warning; the web UI
      grows a permanent amber strip; the presentation terminal grows a chip;
      `stats` shows the mode per column.  **Four surfaces, all of them.**
- [ ] `home all` → all five find their edge.  Time one: a pass is ~7.5 s at
      homing speed and a simulated drum must take the same ~7.5 s, because the
      model uses the real 272000/33 µsteps/rev.
- [ ] Watch the clock run for a few minutes on the web UI.  Every flip, every
      land-on-tick, every `go` event is the real scheduler against the real
      control core — only the Hall input is modelled.
- [ ] `sim fault 0 slip 200` → column 0 reports a slip at its next edge, re-homes,
      recovers.  `stats` shows `resync_major` up and `faults` unchanged or +1
      with cause `slip`.
- [ ] `sim fault 0 miss 2` → two edges suppressed.  The column must classify **jam**
      and **stop without retrying** — that is the whole point of the
      classification.  Banner reads MECHANICAL, not FAULT.
- [ ] `sim fault 0 clear`, `home 0` → back to normal.
- [ ] `col 0 real` with the other four simulated — the build-out configuration.
      Column 0 will fault on `no_hall` (nothing is wired) and park while the
      other four keep running.  **That is the escalation working**, not a bug.
- [ ] `save`, reboot → the modes persist.  A simulated column must **not**
      quietly come back as real.
- [ ] `col 2 disabled` → column 2 parks on blank, is left out of every frame,
      and is reported as configuration rather than as a fault.  The clock keeps
      running with a hole.

### 18. Maintenance mode (spec §5.9) — **DONE 2026-08-23**, see step 17

- [ ] `maint on` → the banner says MAINTENANCE, frame scheduling stops, nothing
      moves on its own, and automatic re-homing is off.
- [ ] `go 0 12` and `cal 0 +10` still work — manual control is the point.
- [ ] Reboot while in maintenance → comes back **still in maintenance**, does
      **not** home, and leaves EN released.  Pulling power mid-repair must not
      restart the display on top of your hands.
- [ ] `maint off` → re-arms and re-homes all five.

### 19. The EN limit, understood on the bench (spec §5.8)

Worth doing once with a meter, because it changes what you can safely do with
your hands in the mechanism:

- [ ] With a column parked or faulted, measure the motor current.  It is still
      drawing TMC2209 **standstill current** — stopping a column stops it
      *stepping*, not holding.
- [ ] `en 0` → now it is actually released, **and so are the other four**, because
      EN is one GPIO across all five drivers and the pin map has exactly one
      spare non-strapping GPIO (24).  Per-column de-energize does not exist and
      is not coming.
- [ ] Therefore: before touching a drum, `maint on` (which releases EN), not
      "wait for it to stop moving".

### 20. The fault thresholds are UNVALIDATED — the sim proves nothing about them

**Read this before trusting any classification you have seen so far.**

Everything in steps 17–19 was exercised against `motion/sim_drum.h`, a drum
model written from **the same assumptions as the classifier that reads it**.
The sim asserts a Hall edge inside a window at a position the classifier
expects, and suppresses edges on command. So of course `miss` classifies as
`jam` and `slip` classifies as `slip` — the model was built to produce exactly
the observations the thresholds were written against. That is circular. It
demonstrates that the *plumbing* works: the cause propagates, the escalation
fires, the retry is or is not attempted, the UI says the right thing. It
demonstrates **nothing** about whether the thresholds match real mechanics.

Three numbers are guesses until a real column exists:

| constant | current value | what it assumes |
|---|---|---|
| `motion.hall_tol` | 41 µsteps (¼ flap) | edge repeatability, never measured (step 6) |
| slip threshold | > 1 flap (165 µsteps) from expected | that a real card catching moves the edge by more than a flap |
| `edge_overdue` (jam) | last edge + 1.5 revolutions | that a stopped drum is distinguishable from a slip within ~3.1 s at 15 flaps/s |

**The first real column must be provoked deliberately**, and the classifications
recorded. Do these with the drum loaded, at `flaps_s_normal`, with `stats` open:

- [ ] **Hold the drum by hand mid-move.** Grip the spool rim and stop it while
      the motor keeps stepping. Record: how long until a fault, which cause,
      and whether the motor was still driving when it latched. Expected `jam`.
      - Result: cause = ______, time to latch = ______ s
- [ ] **Let go part-way through**, so the drum resumes but has lost
      registration. Expected `slip`, retried, recovered.
      - Result: cause = ______, `err` at the edge = ______ µsteps
- [ ] **Slip the pinion on the shaft** (loosen the grub screw a little, or brace
      the drum lightly so a tooth skips). This is the case the `slip` cause
      exists for and the one the sim models least honestly.
      - Result: cause = ______, `err` = ______ µsteps, recovered? ______
- [ ] **Unplug a Hall mid-run.** The edge stops arriving while the drum keeps
      turning freely. **This is the case most likely to be MISCLASSIFIED**: the
      firmware will call it `jam` and stop, because "no edge while stepping" is
      exactly the jam signature, when in fact the drum is fine and a retry would
      be harmless. Record what actually happens — if it reads `jam`, decide
      whether that is acceptable (failing safe) or whether the classifier needs
      another discriminator.
      - Result: cause = ______, acceptable? ______
- [ ] **Unplug a Hall before a cold home.** Expected `no_hall`, three retries.
      - Result: cause = ______
- [ ] **A card catching on the bezel** without stopping the drum — if you can
      provoke it. This is the case the firmware **cannot** see at all (see the
      Notes below) and it is worth confirming that it stays invisible rather
      than producing a spurious fault.
      - Result: ______

**Then re-derive the thresholds from what happened**, in this order: `hall_tol`
from step 6's measured repeatability, the slip threshold from the smallest
real slip worth reacting to, and `edge_overdue` from how long a genuinely
stopped drum takes to be unambiguous. Update spec §5.4/§5.8 and this table with
the measured values, and say in the §17 decision log that they came from the
bench rather than from the model.

Until that is done, treat every fault classification you have seen as
*plumbing verified, physics assumed*.

### 21. MQTT transport (spec 10.2a, 10.3) — **DONE 2026-08-23 (five sim columns)**

No broker needed on the LAN: `tools/mqtt_broker.py` is a stdlib fixture that
speaks enough MQTT 3.1.1 for all of this, including keepalive enforcement so
the Last Will actually fires.

```bash
python tools/mqtt_broker.py                      # on the dev machine
```
```
mqtt mqtt://<dev-machine>:1883                   # on the console
mqtt status
```

Results, on the board:

```
CONNECT  id='ESP32_ddA8E8' keepalive=30s clean=1  will=swan/availability [R]
SUB      swan/cmd/# (qos 1)
PUB      swan/availability   [R] q1  online
PUB      swan/state          [R] q1  {"e":"state",...}
PUB      swan/countdown      [R] q1  {"state":"idle","target":0,"set_by":"unknown","seq":0}
```

- [x] **Commands arrive through the one dispatcher.** `swan/cmd/preset.set` with
      a bare `qmarks` reaches all five columns; `swan/cmd/motion.column` with
      `{"column":2,"mode":"disabled"}` disables column 3. Both answer on
      `swan/event` with the command name and the dispatcher's own result.
- [x] **A bad payload is rejected, not ignored:**
      `{"ok":false,"err":"mode must be real|sim|disabled"}` — the same string
      the web UI gets, from the same validator.
- [x] **No second command grammar.** `swan/cmd/motion.column/2` produces no
      result at all: an argument in a topic segment is refused, never
      half-understood.
- [x] **The deadline says who set it.** `countdown.execute` over MQTT publishes
      `{"state":"running","target":…,"set_by":"mqtt","seq":10}` — exactly the
      four fields §7.3 names, and seq advances so a peer can order two
      retained documents.
- [x] **A RETAINED command is refused on replay.** Note the subtlety: a broker
      *clears* the retain flag when forwarding to an existing subscription, so
      a live retained publish is indistinguishable from a normal one and is
      correctly obeyed. The hazard is the **replay on connect**, which arrives
      with retain=1 — that is what would restart a countdown on every reboot,
      and it is refused. Tested by leaving one retained and forcing a
      reconnect.
- [x] **Cadence.** A *running* countdown published **0** state documents in
      12 s. Before `cd.remaining_s` was excluded from the change comparison it
      published 11 — a retained topic rewritten once a second for 108 minutes.
- [x] **Availability, all four cases:**
      - connect → retained `online`
      - `system.reboot` → retained `offline` in **0.0 s** (a clean DISCONNECT
        makes the broker discard the will, so the firmware publishes it itself)
      - board reset, gone > keepalive → the **will** fires at **50.5 s**
        (45 s after the last packet), retained `offline`
      - board reset, back in < keepalive → **session takeover**, the stale
        will is discarded, and Home Assistant never sees a spurious flap
- [x] **A cold boot with MQTT already in NVS reconnects unprompted** — 2 s
      after the broker appeared, with nothing typed.
- [x] The password never appears in the state payload.

Local gotcha, recorded so it is not rediscovered: asserting **both** DTR and
RTS on the USB-Serial-JTAG port drives the chip into *download mode*, not a
reset — the board then sits silent and off the network. `esptool.py --before
default_reset --after hard_reset read_mac` is the reliable way to reset it.

## Notes

- `step`, `spin` and `revs` are open-loop: they leave the displayed index
  unknown. Re-home before using `go` or `frame`.
- A column that faults re-homes itself up to 3 times before latching FAULT.
  `home <col>` clears it — **unless the cause is `jam`**, which is never
  retried: the drum stopped while the motor kept stepping, and another pass
  drives it straight back into whatever stopped it.  Clear the obstruction
  first.
- **EN is ganged across all five drivers** and there is one spare
  non-strapping GPIO, so per-column de-energize is impossible.  Parking or
  stopping a column stops it *stepping*; its coils still hold standstill
  current.  `en 0` (or `maint on`) is the only true de-energize and it takes
  the whole display with it.
- **The fault thresholds have never met a real drum.** Everything so far was
  tested against a model written from the classifier's own assumptions - see
  step 20. Plumbing verified, physics assumed.
- **The firmware cannot see a card.**  It detects a stalled drum, because that
  moves the Hall edge; it cannot detect a card fluttering, bouncing or failing
  to seat, because the drum position stays correct while the display looks
  wrong.  Anything about flap behaviour is watched, not logged.
