# Bring-up — checklists and results

Results go in this file as they come in. If a bench result contradicts the spec,
the spec is wrong: fix `docs/FIRMWARE_SPEC.md` and append to its §17 decision log
(CLAUDE.md).

## Verification status

Phase 1 builds and its host tests pass. Nothing has run on hardware — the board
has not arrived, so every bench step below is still open.

| gate | status |
|---|---|
| `.\build.ps1 set-target esp32c5` + `.\build.ps1` | **passes** — ESP-IDF v5.5.5, 304 KB, 88% of the app partition free, zero warnings |
| `.\build.ps1 -B build-xiao -DSWAN_BOARD=xiao build` | **passes** — 282 KB. Exercises the alternate pin map and its strapping-pin `static_assert`s |
| host tests (`.\test-host.ps1`) | **2/2 pass** (`test_ring`, `test_motion_math`) |
| `git diff` empty after `gen_ring_table.py` | **clean** — regeneration is byte-identical |
| motion cross-task handoff explicit | **done** — spinlock + request mailbox + relaxed atomics |
| chip revision ≥ v1.0 | **unknown** — board not arrived |
| every bench step below | **not run** — needs hardware |

What passing host tests do and do not prove: `T(i)` rounding, the 8242/8243
revolution alternation, forward-distance costs over all 2500 index pairs, edge
classification thresholds, and that the ramp plus DDA lands on the target
exactly are all verified in software. Nothing about the Hall wiring, the gear
ratio, driver behaviour or flap mechanics is verified by them.

## Phase 1 bench checklist (spec §14.1)

Run with one module on the bench, drivers powered from 12 V, logic from the buck.

### 0. First flash

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

## Notes

- `step`, `spin` and `revs` are open-loop: they leave the displayed index
  unknown. Re-home before using `go` or `frame`.
- A column that faults re-homes itself up to 3 times before latching FAULT.
  `home <col>` clears it.
