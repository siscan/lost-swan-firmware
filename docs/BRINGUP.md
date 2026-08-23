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

Unwired behaviour, which is what this checklist step is really for: every
column runs 1.2 revolutions at homing speed, times out, retries three times on
the 250 ms stagger, then latches FAULT and stops.  Position freezes at 39,560
µsteps, velocity 0, console responsive throughout.  `home 0` clears that
column and leaves the others latched.

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

### 13. Ring upload

- [ ] Upload a deliberately broken `ring.json` (delete a slot).  Expect a
      rejection naming the reason, and **the display must not flinch** — the
      running table is untouched and no file is written.
- [ ] Upload the real `data/ring.json`.  Expect `ring.json applied` in the log,
      the Settings page to report `ring.json` as the source, and the table to
      survive a reboot.

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

## Notes

- `step`, `spin` and `revs` are open-loop: they leave the displayed index
  unknown. Re-home before using `go` or `frame`.
- A column that faults re-homes itself up to 3 times before latching FAULT.
  `home <col>` clears it.
