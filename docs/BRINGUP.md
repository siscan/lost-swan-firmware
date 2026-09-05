# Bring-up — checklists and results

Results go in this file as they come in. If a bench result contradicts the spec,
the spec is wrong: fix `docs/FIRMWARE_SPEC.md` and append to its §17 decision log
(CLAUDE.md).

## THE DRIVE CHANGED — read this before anything else

**2026-09-06: the rim gear is dead.  The motor sits stationary INSIDE the drum
and turns it directly, 1:1** (`docs/ref/DRIVE_CHANGE.md`, spec §3).  With 50
flaps loaded the pinion collided with the card edges at every angular position,
so there was no version of the gear drive that worked.

Three things follow that change what you do at the bench:

1. **`revs 0 10` must read 3200, exactly.**  It is now the single number that
   identifies which machine was built, and the candidates are far apart:

   | reading | machine |
   |---|---|
   | **3200, no spread** | the 1:1 direct drive — what is being built |
   | ~8242 | an 85T/33T rim-gear drum (the original bridge) |
   | ~7555 | an 85T/36T rim-gear drum (designed, never built, never in firmware) |
   | ~8369 | 68T/26T (the stale MECHANICAL_README prose) |

   A direct-drive drum has no residue to alternate, so **any spread at all is a
   finding** — a slipping coupling, a marginal magnet, or a microstep setting
   that is not 1/16.

2. **The coils stay energised, always.**  The drum is unbalanced 3.92 N·cm
   against a 2.2 N·cm detent, so an unpowered drum does not creep — it *slews*
   to its heavy side.  `en_idle_off` is gone.  Every EN drop invalidates
   position and re-asserting EN re-homes; that is now physics, not caution.

3. **THE PARK PIN: pin in ⇒ maintenance mode ON FIRST.**  Service has a pin
   through the idler wall that physically locks a drum.  Enter maintenance
   before the pin goes in, every time.  A commanded move against a pinned drum
   is a deliberate jam — the classifier will catch it and stop without retrying,
   but it should not have to, and a jam you caused on purpose is noise in the
   one record that is supposed to mean something.

## Before you connect the first motor — read this

A cold read of these documents on 2026-08-24 found three things a first-time
bench operator would have had to guess. They are here rather than buried in a
step because getting them wrong costs hardware.

**1. The display homes all five columns automatically, ~100 ms after boot.**
`app_main` asserts EN and calls `home(-1)` (main/app_main.cpp), so a drum
connected to a powered board starts turning before you can type anything, for up
to 1.2 revolutions per attempt and three attempts. **Put the board in
maintenance before you attach the first drum:**

```
maint on
```

Maintenance survives a reboot and a boot in maintenance does not home and leaves
EN released — which is exactly what you want while wiring. `maint off` re-arms
and re-homes everything.

**2. Which rail DIR is tied to is NOT recorded anywhere, and it is a coin
flip.** Spec §2 says DIR is tied at each driver and bench step 3 says to move it
to the other rail if the drum turns the wrong way — but nothing says which rail
to start from, and the rings are **descending**, so "correct" means one forward
flip *decrements* the digit. Decide it with the motor's coupler off the drum, or
in maintenance with `step`, before the drum can jam against the bezel lip.
**Record the answer here when you know it.**

**3. `motion.hall_active_low` is settable now — but NOT from the console.**
Step 2 tells you to change it if the magnet polarity reads inverted. It got a
setter on 2026-08-24 (it had none before, and changing it meant a reflash), and
the setter is on the **Calibrate page**, or over HTTP:

```bash
curl -X POST http://lost.local/api/cmd -d '{"cmd":"motion.params","payload":{"hall_active_low":false}}'
```

then `save` on the console, or SAVE CALIBRATION AND SPEEDS, to persist it.

**There is no console command for it**, which is the awkward part at a bench:
if the board is not on WiFi yet you cannot change it with the serial cable in
your hand. Set the credentials first (`wifi <ssid> <pass>`), or accept a reflash.
Flagged for Nico rather than fixed here — a `hall` sub-command would be four
lines, but this file should not describe a control that does not exist.

**4. The button on GPIO28 is the BOOT strapping pin, and it now does
something.** As of 2026-08-24 a short press EXECUTES the Numbers — it starts or
resets a 108-minute countdown — and a two-second hold RE-HOMES ALL FIVE COLUMNS.
On a bench that is a drum starting to turn while your hands are on it.

Two consequences worth knowing before you lean on the board:

- **In maintenance the button is ignored entirely**, which is one more reason
  step 0 is `maint on`.
- **Holding it while the board powers up or resets may leave the board in the
  serial bootloader** — powered, silent, off the network, looking dead. That is
  the ROM's decision and no firmware can override it. Release it and power-cycle.
  The firmware does the one thing it can: a button already held when it starts
  is ignored until released, so it cannot fire on the boot that follows.

`button` on the console prints the live level, and `button 10` watches for ten
seconds and times the edges — that is how you check a panel button and its loom
before deciding the firmware is at fault.

Two more, less dangerous but confusing:

- **Column numbering is 0-based on the console and 1-based in the web UI.**
  `col 0` on the console is "column 1" on the Calibrate page. The firmware logs
  0-based.
- **`ring` on the console prints ring A only** (columns 1–4). Column 5 carries
  its own table; read it from `GET /api/ring` or the Settings page.

**One wired column and four unwired `real` columns will de-energize the
display.** The four unwired ones fault on `no_hall`, and two or more columns in
trouble drops EN for all five by design (§5.8) — including the one you are
testing. Set the others to `sim` or `disabled` first:

```
col 1 disabled ; col 2 disabled ; col 3 disabled ; col 4 disabled
```

**Wiring is not documented anywhere, and this file cannot invent it.** A cold
read looked for and did not find: the Hall JST pinout (which pin is 5 V, which
is the open-collector output), the coil pairing for the NEMA 17s actually
bought, the TMC2209 Vref procedure for the specific driver modules (`BOM.md`
says "per-vendor formula to ~1.1–1.2 A RMS" and names no vendor), and whether
anything must not be hot-plugged. **Nico: these belong here before the first
motor goes on**, and they are the only bring-up facts nobody in this repo can
derive.

## Verification status

Phases 1–7 build and their host tests pass, and everything through Phase 7 has
been exercised **on the board** — but against simulated drums. **No motor,
driver or Hall sensor has ever been connected.** So every result below marked
done is the firmware working; nothing here is evidence about the mechanism, and
the numbers that describe the mechanism (the gear ratio, `hall_tol`, the
jam/slip thresholds, the usable flap rate) are still open. Steps needing a motor
are marked with an unticked box.

Two things follow from that and are worth saying plainly to whoever reads this
first. **The simulated results are not weak evidence about the mechanism; they
are no evidence at all** — `sim_drum.h` was written from the same assumptions as
the classifier that reads it, so a clean simulated soak proves the plumbing and
nothing else. And **the numbers most likely to be wrong are the ones nothing has
ever pushed back on**: the gear ratio, `hall_tol`, the jam and slip thresholds,
and `flaps_s_alarm`. Steps 3 through 7 exist to settle exactly those, and each
ends in a blank to fill in.

| gate | status |
|---|---|
| `.\build.ps1 set-target esp32c5` + `.\build.ps1` | **passes** — ESP-IDF v5.5.5, DevKitC-1 **1,530 KB** app (40% of the partition free), zero warnings |
| `.\build.ps1 -B build-xiao -DSWAN_BOARD=xiao app` | **passes** — **1,511 KB**. Exercises the alternate pin map and its strapping-pin `static_assert`s |
| LittleFS payload | **208,372 bytes** of a 256 KB budget on a 2048 KB partition — the UI, the presentation terminal, the glyph sheet, the Phase 7 pack, the audio placeholders and `ring.json`.  Gzipped EXCEPT the files the firmware itself reads (`ring.json`, the WAVs): `ring_store` opens the path directly and knows nothing about gzip |
| host tests (`.\test-host.ps1`) | **20 C++ suites + 4 JavaScript suites pass.** The four (`test_flap`, `test_countdown`, `test_logo`, `test_toggles`) need node, which this Windows machine does not have — the runner reports them SKIPPED and Linux CI runs them on every push, so **a green local run is not a green run** |
| `git diff` empty after `tools/ringgen.py` | **clean** — regeneration is byte-identical |
| motion cross-task handoff explicit | **done** — spinlock + request mailbox + relaxed atomics + the `AxisCtl::seq` seqlock (`docs/MOTION_SYNC.md`) |
| chip revision ≥ v1.0 | **v1.2** — production silicon, verified by esptool and the bootloader |
| first flash + boot | **done** — CLI up on COM3, ring.json loads from LittleFS, no revision abort |
| unwired homing fails cleanly | **done** — all five latch FAULT after 3 re-homes; no hang |
| pin map, no strapping conflict | **done** — `pins` matches §2.2 |
| the physical button (GPIO28) | **partly** — the read path, the boot log and `button` verified on the board; **a real press needs a thumb** and has never happened |
| the stand-in bench image | **builds**, reports `0.4.0+devkitc1.bench`, and CI proves the cap compiles and that it refuses to also be a release image |
| the DIRECT DRIVE geometry | **unverified against any drum.** `revs 0 10` must read 3200 exactly; nothing has turned a direct-drive drum |
| Vref / 0.7 A run current | **not measured.** The FYSETC sense resistor is unverified, so the arithmetic is not a substitute (28b gate 3 step 2) |
| the thermal question | **open, and it is the gate.** A NEMA 17 sealed in a PLA drum holding current all day — 28b gate 3 step 6 |
| every bench step needing mechanics | **not run** — needs motors, drivers and Halls |

Three things on that list are **the only work in this file still waiting on
hardware**, and they are worth naming so nobody goes looking for more:

1. **Step 20, the fault thresholds.** The jam/slip numbers have only ever been
   exercised against a model built from the same assumptions. A Hall unplugged
   mid-run is *expected to be misclassified* as `jam`, and that is one of the
   things step 20 is for.
2. **Step 27's real-column soak**, against the 66-minute simulated one.
3. **Step 30b's reveal convergence** with real drums — it measured 2.45–2.48 s
   on simulated ones, but the alarm spin stops in the same place every time in
   simulation and will not on a real drum.

What passing host tests do and do not prove: `T(i)` rounding, the 8242/8243
revolution alternation, forward-distance costs over all 2500 index pairs, edge
classification thresholds, that the ramp plus DDA lands on the target exactly,
clock rendering across DST edges, the countdown schedule from a deadline, and
every web command and the upload validator round-tripped through the JSON
layer are all verified in software. Nothing about the Hall wiring, the gear
ratio, driver behaviour, flap mechanics, radio range behind the aluminium
faceplate, or mDNS on a real LAN is verified by them.

## The first hour, in order

The steps below are numbered but they are not all equal, and a first session
does not run all of them. This is the path:

| # | do | you are answering |
|---|---|---|
| — | `maint on` **before the drum goes on** | nothing turns while you wire |
| — | `col 1..4 disabled` if only one column is wired | four unwired `real` columns drop EN for all five |
| 1 | `pins` | is this the board map you wired to |
| 2 | `hall` + a magnet | polarity, and is the sensor alive at all |
| 3 | `step 0 200` | **which rail DIR is tied to** — with the coupler OFF the drum |
| 4 | `home 0`, `revs 0 10` | **the gear ratio**: 8242 or 8369 |
| 5 | `spin 0 <n> 10`, 10 → 25 | **`flaps_s_alarm`** — watched, not read |
| 6 | `revs 0 20` | **`hall_tol`** |
| 7 | `en 0`, ten minutes | **`en_idle_off`** |
| 8 | `cal`, `save`, then `go 0 0..49` | the blank card, and the right drum on the right column |
| 20 | the physical provocations | the fault thresholds, which are the least-trusted numbers here |

Steps 3, 4, 5, 6 and 7 each end in a number that belongs in the spec, and every
one of them is currently a guess. **Fill in the `Result:` lines as you go** —
they are the whole point of the file, and §17 gets the ones that change a
constant.

Steps 9 through 32 are the later phases and are already done against simulated
drums; they are recorded here as evidence, not as work waiting for you. The two
that are NOT done and need real hardware are **step 20** (the fault thresholds)
and the real-column halves of **step 27** (soak) and **step 30b** (the reveal
convergence).

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

If `magnet` is inverted, the config default is wrong, not the code — set
`motion.hall_active_low` false, by the route in the warning at the top of this
file (Calibrate page or `curl`; there is no console command), then `save`.

If `raw` never moves at all, it is the magnet **face** (A3144 is unipolar —
BOM gotcha #2) or the 5 V supply, not firmware. `hall` prints `raw` straight off
the GPIO bank, so a `raw` that never changes is a wiring fact, not a software
one.

- Result: hall_active_low = ______

### 3. `step 0 200` — direction

- [ ] Drum turns so the **flap fronts fall forward** — that is the one legal
      direction, and on the v3 descending rings it is the direction in which
      the displayed digit DECREMENTS.

If not, move that driver's DIR tie to the other rail. DIR is tied per driver, so
this is a per-column wiring fix; there is no firmware setting for it and coil
order on the JSTs does not need to match.

- Result: DIR rail = ______

### 4. `home 0`, then `revs 0 10` — which machine is this?

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

### 7. ~~`en 0` for 10 minutes with a loaded drum~~ — OBSOLETE

**Struck out 2026-09-06.**  The question was whether a loaded drum creeps with
EN released, so that `en_idle_off` could default true and run the motors cooler.
The direct drive answered it without the test: static imbalance 3.92 N·cm
against a 2.2 N·cm detent means the drum *slews*, not creeps.  `en_idle_off` no
longer exists (spec §5.7).

What replaces it is the thermal question in the other direction — the motor is
now sealed inside a PLA drum and holds current all day — and that is **§28b
gate 3**, below.

### 7-obsolete. (original text kept for the record)

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

### 22. Home Assistant discovery (spec §10.3)

- [ ] Point the display at a real HA broker (`mqtt mqtt://ha.local:1883 user pass`).
      One device, **LOST Swan Timer**, 19 entities.
- [ ] Mode select, Execute and Cancel buttons, Remaining and Deadline sensors,
      Column fault, and — under Diagnostics — **Simulated motion** and
      **Maintenance mode** as *problem* classes, so a simulated display looks
      wrong in HA rather than normal.
- [ ] `mqtt off` → the device **disappears**. Not "unavailable": gone. If a
      ghost is left behind, the retraction did not publish while still
      connected.
- [ ] Pull the display's power → every entity greys out within ~45 s (the
      keepalive), not just the ones that were changing.

### 23. OTA — the survival test (spec §10.4) — **DONE 2026-08-23**

Two prerequisites, both non-negotiable: **two runtime-distinguishable images**
(`PROJECT_VER` carries the board and the flavour), and **`persist`**, which
prints what NVS actually holds. `/api/state` cannot answer this — the countdown
resume is DEFERRED until SNTP, so `cd.target` reads 0 for the first seconds
after any reboot even when the deadline is intact, and reading the display too
early reports a correct rollback as a lost deadline.

Baseline on A (`0.4.0+devkitc1.sim`), five simulated columns:

```
col 3 disabled ; cal 0 37 ; audio vol 55 ; save
countdown.execute 4 8 15 16 23 42
persist  ->  col_mode : sim sim sim disabled sim
             maint    : off
             cd_phase : running     cd_target: 1787535965     cd_seq: 1
             cal      : +37 +0 +0 +0 +0
             image    : 0.4.0+devkitc1.sim (ota_0)
```

- [x] **Swap A → B.** `python tools/ota_upload.py <ip> build_b/…bin` — 1,503,648
      bytes accepted in **5.8 s**, written to `ota_1`, rebooted.
      - `Loaded app from partition at offset 0x2a0000` — the swap happened.
      - **No `nvs partition unusable (…); erasing`.** Its absence is a REQUIRED
        assertion, not a nicety: after that erase everything reads back as a
        plausible default and a board saved all-simulated starts driving unwired
        hardware.
      - `persist` → **byte-identical** apart from `image: 0.4.1+devkitc1.sim
        (ota_1)`. col_mode, maintenance, the deadline, seq and the calibration
        all survived. This needs no clock.
      - `ota status` → `verdict: confirm`, pending cleared.
- [x] **Roll back B → A.** `ota rollback` → `Loaded app from partition at
      offset 0x20000`, and `persist` is **identical to the baseline including
      the image line**. This is the direction the spec's rollback claim had
      never been exercised in.
- [x] **Refusals, on the board:**
      - the XIAO image onto a DevKitC-1 → `400 wrong_board`. Every check IDF
        makes — magic, SHA256, chip id, chip revision — PASSES for it; only the
        version tag can tell, and it would drive STEP on the wrong GPIOs.
      - `data/ring.json` posted as firmware → `400 bad_image` in 0.1 s.

### 24. The watchdog now panics (spec §10.4)

- [ ] `CONFIG_ESP_TASK_WDT_PANIC=y`, timeout 30 s. Confirm a normal boot does
      **not** trip it, including the first boot on a **blank storage
      partition** — that is a LittleFS format, ~512 sector erases with the
      cache off, and the one path that starves even the idle task. It is
      unsubscribed across exactly that call.
- [ ] Confirm an OTA write does not trip it either: 4 KB chunks with
      `OTA_WITH_SEQUENTIAL_WRITES`, one sector erase each.
- [ ] To prove the panic actually fires, add a temporary `while(1){}` to the
      modes task, flash, and watch it reset — then remove it. Do this once, on
      the bench, never on a wall.

### 25. Audio (spec §9) — **DONE 2026-08-23 (placeholders)**

The five cues that ship are **synthesized placeholders** (`tools/gen_audio.py`),
not the show's sounds. They are deliberately distinguishable by ear so a bench
test can tell which fired without looking at the console.

- [x] `audio status` → all five present, 0 underruns.
- [x] `audio play warn_4min` / `system_failure` / `audio stop` — the alarm loops
      and stops on command.
- [x] Through the dispatcher: `audio.play warn_1min` → ok; a cue with no file →
      `no such cue, or no file for it` (refused, rather than reporting success
      and playing nothing); `audio.volume 140` → `volume must be 0-100`.
- [ ] **With the amp wired**: check the gain curve is usable across the whole
      slider (it is perceptual, not linear), that the MAX98357A does not hiss
      between cues, and that `system_failure` at full volume does not clip.
- [ ] **Time each cue against a watch.**  Not a formality: until the Phase 4/5
      review every cue played **84 bytes** — about two milliseconds — and
      `audio play` still returned ok, `audio status` still listed five files,
      and with no amplifier wired there was nothing to hear.  A cue that is
      present, valid and silent looks exactly like a working one from the
      console.  `audio status` now prints the duration; check it matches.
- [ ] Replace a cue from Settings → Audio and confirm it takes effect with **no
      reflash**. A truncated or non-PCM upload must leave the previous cue
      playable.  Do it once **while that cue is playing** — LittleFS will not
      rename over an open fd, so the upload has to stop the player first.
- [ ] Run a countdown to zero and watch the choreography: cue at 4:00, at 1:00,
      the alarm at zero, `countdown.zero_hold_s` then the spin, then the reveal
      frame, then the `reveal` event when the last column confirms it.

### 25b. The Update page (spec §10.2, §10.4) — **DONE 2026-08-24**

- [x] The page reports the running image, which slot it is in, and whether it
      has confirmed itself — all three live from the state document.
- [x] Upload a **bogus** file from the page: refused with the verdict and the
      reason on the page (`refused (bad_image): not an ESP-IDF application
      image`), the button re-enables, nothing reboots.
- [x] Upload a real image: **1,513,376 bytes in ~13.5 s**, `ota_0 → ota_1` and
      back again, each time returning to a display that answers.
- [x] The pending window is **~12 s from boot** (the watcher confirms as soon as
      the invariants are met), of which ~8 s is after the board starts answering
      HTTP.  Long enough to see, short enough that catching it in a browser
      takes deliberate timing — worth knowing before you go looking for it.
- [ ] **With a wrong-board image**: the refusal must name it, and `?force=1`
      must be required.  Then flash back over USB.
- [ ] Press REBOOT *while an image is pending* and confirm the page asks first —
      that reboot is what triggers the rollback.

### 26. Provisioning (spec §10.1)

- [ ] `wifi clear` and reboot → SoftAP `LOST-Swan-xxxx` appears, **open**.
- [ ] A phone joining should pop the sign-in sheet by itself; if it does not,
      `http://192.168.4.1/` must work. Try iOS, Android and Windows — each
      probes a different URL and wants a different answer.
- [ ] The page that appears must be the **setup page**, not the control panel.
      The control panel on an isolated AP hangs on `/api/ring` and a WebSocket.
- [ ] Enter credentials → saved, joins **without a reboot**, and the AP goes
      down only once the STA has an IP.
- [ ] **Mistype the password on purpose.**  The AP must stay up, the page must
      come back with the form after ~45 s, and a second attempt must work.  This
      is the path that locked the user out until the Phase 4/5 review: saving
      credentials took the access point down with it, over the very access point
      the phone was using.
- [ ] **The case that matters:** with credentials present, take the router down
      for ten minutes. The display must stay a clock, keep retrying, and must
      **NOT** enter AP mode. A wall display dropping off the LAN because a
      router rebooted is worse than the outage.
- [ ] `wifi.provision` on demand brings the portal up with credentials present —
      that is the recovery path when the SSID has changed.
- [x] **On a 5 GHz network**, `wifi.provision` used to PANIC the board (the AP
      channel was hard-coded to 1 and one radio cannot straddle two channels).
      Fixed 2026-08-24; the log now reads *"portal follows the station onto
      channel 153"*.  Re-check this on whatever network the display ends up on —
      a 2.4 GHz-only router exercises the other branch.

### 27. Soak (spec 15 phase 6) — **first runs DONE 2026-08-24, simulated**

```
soak start 0 20        # until stopped, at 20 flaps/s
soak                   # the report
soak stop
```

- [x] Nine minutes, five simulated columns at 20 flaps/s: **~120 wraps and
      ~6,000 flips per column**, zero major resyncs, zero faults, and
      `hall_to_hall` pinned at **8242–8243** with a worst edge error of **one
      microstep**.  That is spec §3's 272000/33 = 8242.42 showing up exactly as
      §5.3 says it should: one `resync_minor` per wrap IS the 0.42 residue being
      absorbed at each edge, not a defect.
- [x] **66 minutes, sampled every three minutes** (the long run, 2026-08-24):
      **~1,000 wraps per column** - about 8.2 million microsteps each, 41 million
      across the display - still zero major resyncs, zero faults, `hall_to_hall`
      still 8242-8243, worst edge error still **one microstep**, and the heap
      flat at 66-71 KB with the minimum unchanged from boot.  Nothing drifts and
      nothing leaks over an hour of continuous motion.
- [ ] **With one real column**, the same run.  This is the measurement that
      matters: the simulated drum was written from the same assumptions as the
      classifier, so a clean sim soak proves the plumbing and nothing about the
      mechanism (see step 20).
- [ ] Overnight, then read `curl http://lost.local/api/soak` in the morning.
      Watch `heap_min` against `heap_start`: a steady fall is a leak, a sawtooth
      is normal traffic.

### 27b. The journal under a burst — **DONE 2026-08-24**

- [x] 500 mode changes in 37 s, which is well past the 400-entry cap: the board
      runs straight through it, the journal rotates, `GET /api/journal` returns
      exactly 200 whole objects, and the heap does not move.
      **It did not, the first time**: rotation read every line into memory and
      panicked the board (`reset=panic`, twice), and an unlimited
      `GET /api/journal` did the same from the other side.  Both are two passes
      over the file with one 256-byte buffer now.  Worth repeating after any
      change to the journal, because the failure looks like a reboot with no
      obvious cause.

### 27c. Escalation, and what happens after it — **DONE 2026-08-24**

The one sequence nobody had run to the end.  Five simulated columns, then:

```
sim fault 0 miss 4
sim fault 1 miss 4
go 0 10
go 1 10                # two jams -> two faults -> escalation
stats                  # "drivers disabled"
sim fault 0 clear
sim fault 1 clear
en 1                   # the recovery
stats
```

- [x] Two faults drop EN for all five, as §5.8 requires — `drivers disabled`.
- [x] **The display then STAYS stopped.**  It did not before: `drop_enable`
      posts a Stop, and 50 ms later the frame scheduler's convergence pass
      re-commanded every column, so the axes carried on stepping into dead
      drivers and published faces the drums never reached.  EN is a scheduler
      hold now.
- [x] **`en 1` re-homes all five**, with the log line saying why —
      *"EN re-asserted after an escalation - re-homing all five, because nothing
      knows where the drums are now"*.  It did not before, and could not: that
      Stop leaves every axis Unhomed, the scheduler skips Unhomed columns and
      `go` refuses them, so the display came back energized and permanently
      still behind a banner promising a retry that had been cancelled.
- [x] While EN is down, `preset.set` and `motion.spin` are **refused with a
      reason** rather than answered `ok`; `motion.enable` is not blocked,
      because it is the way out.
- [x] A recovery reaches the journal: inject `sim fault 2 slip 400`, and
      `GET /api/journal` gains `{"e":"recover","col":2,"d":"after 1"}`.  That
      line was unreachable until today — the retry counter is cleared at the
      hall edge, several hundred milliseconds before the event that carries it.

### 28. Power loss (spec 15 phase 6) — **DONE 2026-08-24**

Twelve cuts, each a hardware reset with no shutdown path — the same thing as
pulling the plug as far as NVS and LittleFS are concerned.

- [x] Ten points through a countdown: just after EXECUTE, mid quiet phase, one
      tick before the seconds wake, inside the live-seconds window, at the
      1-minute cue, five seconds from zero, and after a cancel.  **Every one**
      came back with the deadline intact to the second, the five column modes
      intact (including a *disabled* one), and the calibration intact.
- [x] The two transient phases, caught live because `countdown.set_target`
      refuses a past epoch: power cut **during the zero hold** and **during the
      alarm spin**.  Both woke silently into the reveal — no cue replayed,
      nothing spinning.  That is the §17 rule (the cues and the spin belong to
      the real zero moment) holding under a power cut rather than a reboot.
- [x] The journal recorded the whole campaign: every zero, every boot, every
      cancel, with timestamps and `seq`, across all twelve cuts.
- [ ] With motors attached: a cut mid-move leaves the drum wherever physics
      left it, so the FIRST thing after a power-loss test is `home all` — the
      firmware cannot know where a drum stopped.

### 29. Fault-path matrix (spec 5.8/5.9/7.4) — **DONE 2026-08-24, simulated**

Run `matrix_board.py`, or by hand:

- [x] Two columns faulting **while still retrying** → EN drops for all five.
      (It did not before: a retryable fault stores `Unhomed`, so neither column
      counted itself and the rule could only fire ~22 s later.)
- [x] After an escalation, **no column is still MOVING** — dropping EN posts a
      Stop rather than only writing the pin.
- [x] EN is re-assertable from the network (`motion.enable`), not just `en 1` on
      the console, and `sys.drivers_enabled` says which state it is in.
- [x] One column **disabled and latched** plus one real fault → EN stays up.  A
      disabled column is configuration, not a vote.
- [x] **Re-enabling** a disabled column homes it by itself and reaches a known
      index.  It never did; across a reboot it came back unhomed and silently
      never closed the hole.
- [x] **Disabling** an idle column parks it on blank *before* its drive bit goes
      away — verified by watching a `qmark` become `blank`.
- [ ] The same six on a real column, where EN actually de-energizes something.

### 30. Zero and failure semantics, verified not redesigned — **DONE 2026-08-24**

Asked for by the terminal prop's integration pass: confirm what the display
already does at zero, on hardware, rather than assume it. Five simulated
columns, MQTT against `tools/mqtt_broker.py`, deadlines armed 12 s out with
`countdown.set_target`.

```bash
python tools/mqtt_broker.py                       # on the dev machine
```
```
mqtt mqtt://<dev-machine>:1883                    # on the console
```

- [x] **`countdown.failure_timeout_s` defaults to 0 and at that default the
      reveal holds indefinitely.** Read off the board: `failure_timeout_s = 0`,
      and the display sat in `reveal` until something else moved it. The key is
      kept, not removed.
- [x] **Set non-zero, it fires AND publishes the transition.** With
      `failure_timeout_s = 8`, the display left the reveal for the clock exactly
      8.0 s after entering it, and the retained topic followed:

      ```
      PUB  swan/countdown  [R] q1 {"state":"reveal","target":1787609294,"set_by":"ui","seq":1}
      PUB  swan/countdown  [R] q1 {"state":"idle","target":1787609294,"set_by":"ui","seq":1}
      ```

      That `idle` publish is what a peer following retained state needs in order
      to leave SYSTEM FAILURE when the display leaves it by itself.  Restored to
      the default of 0 afterwards.
- [x] **The phase timings are exact against the DEVICE's clock**, measured by
      polling `/api/state` rather than by watching MQTT:

      ```
      phase -> running  at target-10.912 s
      phase -> zero     at target+0.065 s
      phase -> spin     at target+3.039 s     (zero_hold_s = 3)
      phase -> reveal   at target+9.035 s     (spin_s = 6)
      all five settled  at target+9.176 s
      ```

      **Worth knowing, because it cost me an hour:** the same run measured
      through MQTT reads the zero->spin gap as **2.30-2.40 s**, not 3.00. That
      is publish skew on the retained `swan/countdown` topic, not a firmware
      timing error — the choreography is driven by absolute arithmetic from the
      deadline and is exact. A peer that schedules off `target` + the timing
      keys (which is what the prop does) is unaffected; a peer that timed off
      the arrival of the `zero` publish would be wrong by up to ~0.7 s.
- [x] **Every phase transition publishes retained `swan/countdown`**, including
      the ones that matter for recovery:

      ```
      PUB  swan/countdown  [R] q1 {"state":"running","target":1787607365,"set_by":"ui","seq":4}
      PUB  swan/countdown  [R] q1 {"state":"zero",...}
      PUB  swan/countdown  [R] q1 {"state":"spin",...}
      PUB  swan/countdown  [R] q1 {"state":"reveal",...}
      ```
- [x] **`countdown.execute` from the reveal state is ACCEPTED as a fresh
      108:00**, not rejected: `{"ok":true}`, phase `running`, remaining 6477 s
      two seconds later, and `seq` advanced 1 -> 2. The new retained deadline
      publishes immediately, which is what pulls a peer out of SYSTEM FAILURE.
- [x] **`swan/event` carries a command result only for MQTT-origin commands.**
      Measured both ways in one window: a `clock.format` over HTTP produced
      nothing on `swan/event`; the identical command over `swan/cmd/` produced
      `{"cmd":"clock.format","res":{"ok":true}}`. So the prop's own execute gets
      its result, and an execute from the web UI or the physical button does
      not — the retained `swan/countdown` is what carries that case, and it
      publishes for every origin. Recorded because it looks like a bug from one
      side and is a deliberate asymmetry from the other.
- [x] **The `reveal` announcement.** `{"e":"reveal","seq":N,"t":...}` on both
      `swan/event` and `/ws` when the last column confirms the reveal frame,
      plus one `reveal` line in the journal.
- [ ] **With a real column**, the reveal convergence again. It landed in
      **0.146 s** in this run because `countdown.reveal` was still unset at the
      time, so the reveal frame was all blanks and blank was one flip from where
      the spin stopped. It has been set since — step 30b re-measured it at
      **2.45–2.48 s** with the canon five — and on a real drum it can be up to a
      full 49-flip wrap (~3.3 s at 15 flaps/s). That range is exactly why the
      beat is announced rather than estimated.

### 30b. The canon reveal frame — **DONE 2026-08-24**

`countdown.reveal` was the last unset config value. The five are staff, spiral,
obelisk, bird, branch, columns 1 to 5 (spec §11, Lostpedia).

- [x] **Verified against the manifests before setting anything.** All five are
      `art_source: DJ original SVG (on-screen canon)` in BOTH rings, which is
      what separates them from the near-siblings the sheet also carries:
      `staff` from `hook` (curved hook, master #13), `branch` from `fork`
      (forked branch, master #14), `obelisk` from `boundloop` and `vloop`.
      Neither rejected sibling is on column 5's reduced ring at all.
- [x] **Column 5 carries `branch`**, at ring B slot 35 — the one thing that had
      to be true, and the reason to check before setting rather than after.
- [x] Set by NAME through the dispatcher and persisted:
      `cfg.reveal = ['staff','spiral','obelisk','bird','branch']`.
- [x] **`preset.set reveal` renders it**: `idx = [37, 36, 35, 34, 35]`,
      `face = [staff, spiral, obelisk, bird, branch]`. Note column 5 is **35**,
      not ring A's 33 — the by-name resolution earning its keep.
- [x] **The countdown-zero landing renders the same frame**, `[37,36,35,34,35]`.
- [x] **Convergence re-measured with five real glyphs**: 2.479 / 2.454 / 2.480 s
      from the reveal phase to all five confirmed, against 0.146 s and 1.695 s
      when the frame was blanks. Whole failure beat, zero to confirmed:
      **11.45–11.52 s**. The tight spread is the simulated drum stopping in the
      same place every run; a real one will not.

### 31. The terminal prop's presence — **DONE 2026-08-24 (simulated peer)**

`swan/prop/terminal`, published by the prop, read-only here.

```bash
python <scratchpad>/mqtt_peer.py <broker> prop 1.0.0 30
```

- [x] A retained birth `{"online":true,"fw":"1.0.0"}` is **obeyed**, and this is
      the subtle one: the inbound path refuses retained messages, because a
      retained `swan/cmd/...` is a countdown that starts itself in an empty
      room. That gate was topic-blind and would have eaten the prop's birth
      message — a presence document is retained BY DESIGN, which is how a
      display that boots later learns the prop is already there. The refusal is
      now scoped to command topics.
- [x] The last will `{"online":false}` arrives when the peer drops without a
      DISCONNECT, and Diagnostics reads `terminal prop: OFFLINE (last will
      received)`.
- [x] Never having seen a prop reads differently from having seen one go away —
      `never seen on swan/prop/terminal` against `OFFLINE`.
- [x] A malformed document is ignored whole and the previous presence stands.

### 32. The physical button (spec §2.5, Q6) — **DONE 2026-08-24**

- [x] A short press executes the Numbers: `swan/countdown` shows
      `"set_by":"button"`, and the journal line reads
      `{"e":"execute","by":"button","d":"4 8 15 16 23 42"}`.
- [x] A hold of two seconds re-homes all five, once, and the release afterwards
      does nothing.
- [x] In maintenance the button is ignored entirely, with one log line.
- [x] **Held at startup: nothing happens until it is released**, and the boot
      log says so. This is the strapping-pin rule (§2.5) and it is the one
      behaviour that cannot be discovered safely on a wall-mounted display.
- [ ] **On the real enclosure**, with the external button in parallel with BOOT
      and a long loom. Watch for: a press that lands in the bootloader instead
      (release and power-cycle), and false triggers from loom pickup — the
      internal pull-up is enabled, but a metre of unshielded wire next to five
      stepper motors is a different proposition from a pad on a dev board.

### 33. Phase 7, the presentation pack — **DONE 2026-08-24 (browser, not bench)**

Recorded here for completeness rather than as bench work: it is entirely
browser-side and the ESP32 pays only the LittleFS bytes. Verified against the
board at 375x812 and 1920x1080, with no horizontal overflow at either.

- [x] **Protocol mode is inert outside the live window**, which is the point of
      it: with 380 s remaining, typing the whole Numbers and Enter produced
      nothing on screen, sent no command, and did not open the eggs.
- [x] **The SYSTEM FAILURE flood matches the §7.3 cadence contract**, measured
      live: pure 14-character repeats with no separator and no newline, 108
      repeats in 10.8 s (the 100 ms tick), stopping when the phase leaves
      zero/spin/reveal, and the paper NOT cleared afterwards.
- [x] **The beats are timed off the deadline, the landing off the `reveal`
      event** - watched it go SEALING to SEALED as the event arrived.
- [x] **The Pearl log prints the device's real journal**, honouring the frozen
      format: uptime stamps for `t: 0`, column 0 emitted, the device's timezone.
- [x] **Chess: 27/27 self-test in a browser on the board**, including perft
      20 / 400 / 8902 from the initial position, kiwipete 48 / 2039, and
      checkmate detection (which is what opens the Chang menu).
- [x] **The boot logo is finished art** (2026-08-25). The supplied vector
      replaced the constructed generator wholesale: frame, Later-Heaven trigram
      ring, disc, swan and the DHARMA wordmark, all pre-proportioned in one
      200x200 system. The swan is genuinely stroke-drawn along centreline
      spines carrying a width per vertex, then crossfaded to the fill; measured
      on the board, the spines go 0% at 1.6 s to 100% at 2.75 s.
      `test/host/test_logo.js` checks the drawn ring against
      `docs/ref/swan_trigrams.md` on every push, and **the four non-palindromic
      trigrams - Dui, Gen, Zhen, Xun - are what it is really checking**: the
      other four read the same inside-out, which is why the wrong ring survived
      a review. **Do not re-derive the order from bagua theory.**

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


---

## 28b gate 3 — THE STAND-IN BENCH SESSION

**This is the first firmware in this project that will drive a real motor.**
Everything since 2026-08-23 has run on real silicon against modelled drums; this
session puts current through a coil and ends with a hand on a warm motor case.

**What it is gating.** Mechanical will not buy parts for the in-drum motor
design until this passes. The question is narrow and physical: *a NEMA 17 is
sealed inside a PLA drum that softens at 55–60 °C, and the clock holds position
99 % of the day — does it cook?*

**What you need on the bench**

- one column: drum on the **printed PLA stand-in axle**, one NEMA 17 inside it,
  one **FYSETC TMC2209**, one A3144 Hall and its magnet at R52
- the ESP32-C5 board, 12 V to the driver, USB to the PC
- a multimeter with a fine probe or a clip
- a small screwdriver for the driver's trimpot
- **a timer, and an hour you are not going to need the bench for**

**The build is its own flavour, and it is not optional.**

```bash
.\build.ps1 -B build-bench -DSWAN_BENCH=ON set-target esp32c5
```

```bash
.\build.ps1 -B build-bench -DSWAN_BENCH=ON app
```

```bash
.\build.ps1 -B build-bench -DSWAN_BENCH=ON -p COM3 app-flash monitor
```

It reports itself as `0.4.0+devkitc1.bench` in the boot log, in `sys.version`
and to an OTA. **The show spin is absent from this image.** Every commanded
speed is clamped to **50 flaps/s = 1 drum rev/s** at the one place speeds enter
the motion layer, so a value from NVS, a Settings slider or an MQTT peer cannot
lift it; `bench spin` above the cap is refused outright rather than quietly run
slower. The stand-in axle is printed and the cap is a safety contract, not a
config default. A build that can exceed it is a different build.

---

### Step 1 — wire it, and check the Hall before the motor moves

- [ ] `pins` — confirm the map, and that **DIR=GPIO24** appears
- [ ] `hall` — wave the magnet past the sensor; `magnet=YES` when present.
      If it reads inverted, `motion.params {"hall_active_low": false}` and
      `save`. Do this before homing, or homing hunts for an edge that reads
      backwards.
- [ ] `col 0 real` — the bench soak **refuses a simulated column**, on purpose.
      A modelled drum would produce a beautiful hour of logs and answer nothing.

### Step 2 — SET VREF BY MEASUREMENT — 0.7 A RMS

**Do this before enabling the driver, with the motor unplugged.** The usual
`Vref = I_RMS × 2.5 × R_sense` arithmetic is only as good as `R_sense`, and the
FYSETC modules' sense resistor value is **unverified** — that is the whole
reason this is a measurement and not a calculation.

1. Power the driver from 12 V with **the motor disconnected** and EN released
   (`en 0`).
2. Put the meter's negative on driver GND. Touch the positive to the **trimpot
   wiper** — on a FYSETC TMC2209 that is the metal screw head itself. Do not
   short it to the neighbouring pads; a slipped probe here kills the driver.
3. Turn the pot and read Vref directly.
4. Target, for **0.7 A RMS** with the common 0.11 Ω sense resistor:
   `Vref ≈ 0.7 × 2.5 × 0.11 = 0.193 V`. If the module is 0.15 Ω it is 0.263 V.
   **Which it is, is what you are about to find out** — set the pot to the 0.11 Ω
   figure, then confirm the actual current in step 5 and adjust.
5. Record it here, because the next session cannot re-derive it:

   ```
   driver #1 serial / marking : ______________________
   sense resistor (if legible): ______________ ohm
   Vref set                   : ______________ V
   measured phase current     : ______________ A RMS   (step 5)
   date / who                 : ______________________
   ```

- [ ] Vref set and **written down above**

### Step 3 — direction: the drum must turn the DESCENDING way

The motor now faces the opposite way inside the drum, so which DIR level gives
the show's sense is not knowable on paper.

- [ ] `en 1`, `home 0`
- [ ] `go 0 1` and watch: **one forward flip must DECREMENT the displayed
      digit** (spec §4 — the rings are descending).
- [ ] If it increments: `dir 1`, `home 0`, try again.
- [ ] `save` — persists with the calibration.

Refused while a column is moving, deliberately: the driver samples DIR on the
next STEP edge, so flipping it mid-move walks the drum backwards.

### Step 4 — homing and the machine's identity

- [ ] `home 0` completes without a fault
- [ ] `revs 0 10` — record it:

   ```
   hall_to_hall (10 revolutions): ______________
   spread (hi − lo)             : ______________   <- should be ZERO
   ```

**Expect 3200 with no spread.** Anything else, stop and read the table at the
top of this file — the number tells you which machine you are holding. A spread
on a direct drive is a slipping coupling, a marginal magnet, or a microstep
setting that is not 1/16.

### Step 5 — confirm the current you actually set

- [ ] With the motor connected and a move running, measure one phase current
      (clamp meter, or a low-side shunt if you have one rigged).
- [ ] Compare against the 0.7 A RMS target and adjust Vref; **re-record both
      numbers in step 2's block.**

If your meter cannot do this honestly, say so in the block rather than writing
a number you inferred. An unverified current invalidates the thermal result,
which is the only thing this session exists to produce.

### Step 6 — THE ONE-HOUR HEAT SOAK

```
bench soak 0
```

Sixty minutes, one flap per second, on column 0. Leave it alone. `bench` prints
progress; `bench stop` aborts.

**Why one flap a second, when the clock moves every fifteen minutes.** The heat
is almost entirely **holding** current: a flap at 15 flaps/s occupies ~67 ms, so
even at one flap a second the coils are holding for >93 % of the hour, and at
the clock's real cadence >99.99 %. Those are the same thermal question. What the
faster tick buys is *motion* data — 3600 flaps and 72 drum revolutions of Hall
edges, resyncs and edge errors — so the hour answers two things instead of one.
If you want the literal clock duty, `bench soak 0 60 900`.

- [ ] soak ran the **full hour** — a short run is not a shorter answer, it is no
      answer, and the firmware says so rather than printing a verdict prompt

**At the end the firmware stops and asks.** Put a hand on the motor case:

```
comfortable to hold           -> well inside margin
hot but you can keep it there -> around 45-50 C, marginal
you snatch your hand away     -> FAIL, go to UART/IHOLD (spec 5.7a Plan B)
```

Record the verdict — an unrecorded verdict is an hour that has to happen twice:

```
hand-on-case verdict : ______________________________________
drum exterior (also) : ______________________________________
flaps / revs         : ______________  /  ______________
hall_to_hall min..max: ______________  worst edge err: ________
resyncs minor/major  : ______________  faults: ________
heap start / min     : ______________  /  ______________
date / who           : ______________________
```

### Step 7 — the slow spin: runout and wire routing

```
bench spin 0 25 20
```

Twenty seconds at 25 flaps/s (half the cap). Then, if that looked right:

```
bench spin 0 50 20
```

One drum revolution per second — the fastest this image will go.

- [ ] no visible wobble or runout at the drum rim
- [ ] the motor leads exiting the Ø6 support-tube bore do not snag, chafe or
      wind up (fixed motor, no slip ring — the leads should simply sit there)
- [ ] nothing rubs the flaps
- [ ] `bench spin 0 400 5` is **REFUSED**, with the reason. Try it once so you
      have seen the refusal work rather than trusting that it does.

### What passing means, and what it does not

Passing gates the **purchase**. It does not verify the machine: this is a
printed stand-in axle, one column, and an hour. It says the motor survives its
duty cycle sealed in a drum at 0.7 A, that the drum turns true at 1 rev/s, and
that the geometry reads 3200.

It says **nothing** about the show spin, which this image cannot produce, about
five columns' thermal behaviour in a shared enclosure, or about the aluminium
faceplate. Those come after the real axle.

**If it fails:** spec §5.7a Plan B — UART mode, `IHOLD` ≈ 15 %. Read the pin
cost there first; it is not free on either board map.
