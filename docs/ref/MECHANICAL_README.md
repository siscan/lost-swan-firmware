# LOST Swan Station split-flap display — build package

Everything designed so far, what's settled, what isn't, and why.

---

## 1. What it is

A wall-mounted five-column split-flap display reproducing the Swan station
countdown timer. It runs as a **clock 99% of the time**, a **five-character
message board** occasionally, and a **108-minute countdown** when you want it.
At zero the columns flip to hieroglyphs.

Faceplate **475 × 218 mm**, laser-cut aluminium. Characters 76 mm tall.

---

## 2. Locked specification

| | |
|---|---|
| Columns | 5, arranged 3 + 2 with a band between (matches the prop) |
| Ring | 50 positions: blank, dash, 0-9, A-Z, AM, PM, 9 glyphs, 1 spare |
| Ring order | **ascending** — one flip per clock tick |
| Flap | 75 × 51.7 × 1.0 mm |
| Character card | 75 × 103.4 mm (two flaps), 76.2 mm cap height |
| Module pitch | 80 mm |
| Inter-card gap | 5 mm (two 2.5 mm module walls) |
| Pin circle | R60, 50 holes Ø2.35, **5.19 mm wall between holes** |
| Spool disc | 4.5 mm thick, rim cut as a gear |
| Drive | printed spur gears, ~2.6:1 reduction, motor inside the module |
| Motor | NEMA 17 + TMC2209, one per column |
| Homing | hall sensor + magnet, calibration offset in firmware |
| Control | one ESP32, web UI hosted on the device, MQTT to Home Assistant |
| Colours | 3 filaments: black, white, red |

### Colour scheme

- **Columns 1-3** — black card, white digits/letters, **red** glyphs
- **Columns 4-5** — white card for digits/letters, **red card** for glyphs,
  black characters throughout

Two flaps per drum in columns 4-5 straddle the white→red boundary and are
two-tone front-to-back. Trivial on a toolchanger, four parts in the whole build.

---

## 3. How it works

See `renders/anatomy.png` — everything below is labelled there.

**The flap** is a card with two small pivot lobes. It narrows near the pivot
(the **shoulder pocket**) so the spool disc tucks *inside* the flap's own
width. That's what lets adjacent cards nearly touch — the drum is never wider
than the card. The length of that narrowing (the **notch**) is derived, not
chosen: `sqrt(disc_radius² − pin_radius²)`.

**The spool** is two discs joined by a Ø26 hub, held on a fixed Ø8 shaft by two
688ZZ bearings. Flaps hang from 50 pins at R60.

**The central channel.** Because the pin circle (R60) is larger than the flap
length (46.7 mm), a hanging flap stops 13 mm short of the axis. Nothing sweeps
a Ø26 core running the full length of the drum — which is where the shaft and
bearings live. This only exists at these proportions and it's the single
geometric fact the whole layout depends on.

**The drive.** Gear teeth are cut into the spool disc's own rim (68T). The
motor pinion (26T) meshes in the *same plane as the disc* — inside the module,
not between modules. So the inter-module gap only has to hold the two walls,
which is how it gets down to 5 mm. No separate drum gear, no belt, no
tensioner. Reduction 2.6:1 falls out free.

**The faceplate** has two window cutouts, one per group, no mullions — matching
the prop. A printed **bezel insert** behind it carries a small lip that catches
the top of each fallen flap so it sits flat instead of flopping forward.

---

## 4. Parts

### Ready to print

| File | Notes |
|---|---|
| `coupons/lost_coupons_v2.3mf` | **print these first** — three opacity test flaps |
| `parts/spool_drive_side.stl` | disc with rim gear + hub, bearing seat, hex tenon |
| `parts/spool_idler_side.stl` | as above + magnet pocket, hex socket |
| `parts/motor_gear.stl` | 26T pinion, grub screw boss |

### Provisional — see §6

| File | Notes |
|---|---|
| `parts/flap_body.stl` + `flap_ink_front/back.stl` | notch is sized for the *old* disc |
| `parts/module_wall_drive.stl` / `_plain.stl` | motor position not yet valid |
| `parts/faceplate.stl` | correct outline, window sizes may want trimming |
| `parts/bezel_insert.stl` | first pass, lip geometry untested |

### Not built

- **Shroud** — curved guide over the top and rear of the drum. Load-bearing,
  see §6.
- **Outer enclosure** — box holding the five modules.
- **Flap magazine** — assembly aid, needs regenerating for the bigger spool.
- **Firmware.**

---

## 5. Print settings

- **0.2 mm layers, 0.4 mm nozzle.** Every thickness in the design is a
  multiple of 0.2.
- **First layer 0.2 mm, not 0.25** — the flap's colour boundaries sit exactly
  at z = 0.4 and 0.6 and must land on layer lines.
- **Flaps: 100% infill.** At 1 mm thick the single core layer is the only thing
  stopping the back character ghosting through the front.
- Filament slots: **1 black, 2 white, 3 red**.
- Flaps and discs print flat, no supports.

---

## 6. Open items

**The opacity test gates flap thickness.** Print the three coupons and judge in
normal room light: does white read white over black at 0.2 vs 0.4 mm of ink,
and does the red on the back ghost through? Result decides 1.0 mm vs 1.4 mm
flaps. Also check the printed pivot lobes for stiffness — that's the known
weak point in every build of this type.

**The flap notch is stale.** Putting gear teeth on the disc rim pushed its
radius from 64 to 70, and the notch must follow:

| disc | notch | protrusion in front of the card |
|---|---|---|
| as built, m=2 z=68 | **36.1 mm** | 10.0 mm |
| finer teeth, m=1.5 z=85 | **25.6 mm** | 5.25 mm |

The current flap has 22.2 mm. Either way it needs regenerating; I'd take the
finer teeth, since 36 mm of notch on a 51.7 mm flap leaves very little
full-width card, and 10 mm of disc sticking out in front of the card would be
visible through the 5 mm gaps.

**The shroud is load-bearing, not cosmetic.** Flaps hanging free sweep a R107
envelope, and the motor at 94 mm centre distance sits inside it. The shroud
holds the pending stack back and shrinks that envelope to roughly R85, which is
what makes the motor position legal. Its radius depends on how tightly 50 flaps
actually pack — a friction question that needs a physical drum in hand. **Until
it exists, module depth and motor position are not final.**

**Module wall dimensions** follow from the shroud, so those two files will
change.

---

## 7. Decision log

Things that got reversed during design, and why — so you don't re-litigate them:

- **Descending → ascending ring order.** Descending gives one flip per
  countdown decrement, ascending gives one flip per clock tick. Clock runs
  99% of the time, so ascending. DJ went descending because his build has no
  clock mode.
- **Minutes-only → tens-of-seconds live.** A 49-flip decrement takes ~2.2 s
  against a 10 s budget. Ones-of-seconds is impossible on a one-way 50-ring.
- **28BYJ-48 → NEMA 17.** Not for speed — the flaps cap that at ~20-25/sec
  regardless. For torque margin and because you wanted the alarm spin.
- **Optical → hall homing.** Optical interrupters need ~10 mm axial; the gap
  is 5 mm. Hall packages are ~1.5 mm. Two of three reference builds use hall.
- **Belt → gears.** Belt needs pulley width plus flanges in the gap. Rim teeth
  on the disc put the mesh inside the module instead, costing zero gap.
- **R28 → R40 → R60 pin circle.** Wall between pin holes went 1.17 → 5.19 mm.
  R60 also creates the central channel, which is what makes the whole drive
  layout possible.
- **Gap 4.3 → 14 → 5 mm.** The 14 mm intermediate was a wrong turn: I had the
  drive gear in the gap instead of on the disc rim.
- **Per-module enclosure → shroud only.** A full shell duplicates the outer
  box's job; the curved guide is the part that does real work.

---

## 8. Reference sources

Three builds were measured, not just read:

- **DJ Harrigan (element14 #481)** — STEP + firmware. Gave flap 75 × 51.7 ×
  1.6, 40 steps/flap, 800 steps/rev, descending ring, the shoulder-pocket
  trick, and the hieroglyph artwork (his vector work — credit him, don't sell
  it).
- **Scott Bezek (`splitflap`)** — OpenSCAD. Gave the notch formula, 5 mm
  hole separation, software calibration, closed-loop home verification.
- **MorganManly (Instructables / MakerWorld)** — Gave 37.9 mm module pitch
  with a 33.1 mm drum, the long buried centre gear, the M3 tie-rod stack, and
  a complete ESP32 web UI worth modelling ours on.

---

## 9. Suggested order of work

1. Print the three coupons. Decide flap thickness.
2. Print two spool halves and ~15 flaps. Assemble on a Ø8 rod, hand-spin it.
   Watch for clean single drops and check lobe stiffness.
3. From that, fix the shroud radius.
4. Regenerate flap (correct notch), module walls, and magazine.
5. Print one complete column and run it open-frame before committing to 250
   flaps.

---

## 10. Parameters file

`scripts/params.py` is now the single source of truth. Every dimension lives
there once; everything else is derived. Run it directly for a full report plus
consistency checks:

    python3 scripts/params.py

The checks are the useful part — they catch the class of bug that produced the
stale flap notch. `FAIL` means a part will not work. `WARN` means it's outside
the reference builds but may still be fine.

Two warnings are expected right now and both trace to the same missing part:
the motor sits inside the free-hanging flap envelope, and the shroud radius
isn't set. Both clear once the shroud exists.

Changing `GEAR_MODULE` or `Z_DRUM` moves the disc radius, which moves the flap
notch automatically — that propagation is the whole point of the file.

---

## 11. Complete parts inventory

Status key: **DONE** = file exists and is current · **STALE** = exists, needs
regenerating · **OPEN** = not built, not blocked · **BLOCKED** = waiting on
something upstream

### Drum

| Part | Status | Note |
|---|---|---|
| flap body + 2 inlays | STALE | notch must go 22.2 → 25.6 mm |
| spool drive side | DONE | |
| spool idler side | DONE | |
| motor pinion | DONE | 33T if regenerated at module 1.5 |
| shaft Ø8 × ~85 | — | cut from steel rod, not printed |
| 688ZZ bearings ×2 | — | bought |
| shaft retainer / collar | OPEN | shaft must be clamped to a wall |

### Module

| Part | Status | Note |
|---|---|---|
| wall, drive side | STALE | motor hole position provisional |
| wall, plain side | DONE | |
| **shroud** | BLOCKED | needs the hand-spin test — see §6 |
| hall sensor bracket | OPEN | magnet sits at R52 on the idler disc |
| motor mount | — | motor bolts straight to the drive wall |
| wire retainer | OPEN | motor + sensor leads, per module |

### Display assembly

| Part | Status | Note |
|---|---|---|
| faceplate | DONE | outline correct; window trim may want a tweak |
| bezel insert | STALE | lip depth follows disc protrusion (5.25 mm) |
| faceplate standoffs / fixings | OPEN | must not show screw heads on the front |
| enclosure — back panel | BLOCKED | depth follows module depth |
| enclosure — top panel | BLOCKED | " |
| enclosure — bottom panel | BLOCKED | " |
| enclosure — end caps ×2 | BLOCKED | " |
| wall mount (French cleat) | BLOCKED | mounts to the back panel |
| tie rods ×4 + hardware | — | M5 threaded rod, bought |

### Electronics

| Part | Status | Note |
|---|---|---|
| ESP32 mount | OPEN | placement follows the enclosure |
| TMC2209 carrier / mounts | OPEN | 5 drivers |
| PSU mount | OPEN | 12 V |
| wiring harness | OPEN | JST chain per module |
| power inlet | OPEN | panel mount |
| speaker + amp mount | OPEN | optional |

### Software / docs

| Item | Status |
|---|---|
| firmware (motion, homing, modes) | OPEN |
| web UI | OPEN |
| MQTT / Home Assistant | OPEN |
| hardware BOM | OPEN |
| assembly instructions | OPEN |

### Tooling

| Part | Status | Note |
|---|---|---|
| flap magazine | STALE | regenerate for the Ø130 spool |

---

## 12. What blocks what

Only one chain is genuinely blocked, and it is three deep:

```
hand-spin test  ->  packed stack radius  ->  shroud
                                              |
                                              v
                                        module depth
                                              |
                        +---------------------+---------------------+
                        v                     v                     v
              final motor position    enclosure panels        wall mount
                        |
                        v
              drive-side wall holes
```

Everything under §11 marked **OPEN** is not blocked by that chain — it's simply
not built yet. Several of those (shaft retainer, sensor bracket, faceplate
fixings, magazine, firmware) can be done at any time.

The reason they weren't done earlier is that the fundamentals moved a lot:
pin circle 28 → 40 → 60, gap 4.3 → 14 → 5, belt → gears, and the drive
relocating from the inter-module gap onto the spool rim. Anything detailed
before those settled would have been scrapped.

---

## 13. v6 revision — response to external engineering review

An independent review (GPT) audited the package and found real problems. All
addressed in this revision:

**STOP-SHIP fixed: single pipeline.** `scripts/generate.py` is now the only
producer of STLs and imports every dimension from `params.py`. It measures each
exported STL against expected dimensions and reports OK/MISMATCH. The old
mismatched parts are in `archive/parts_v5_stale/` — do not print them.

**Root-ligament check corrected.** The old check compared pivot holes against
the gear's *addendum* radius; the load path runs through the *root* radius,
where the ligament is only 0.70 mm. The corrected check FAILs for through-holes
— which is why the pivot holes are now **blind pockets, 1.6 mm deep from the
flap side**, leaving 2.9 mm of solid disc behind the pins. Note: assembly
changes — flaps now flex into pockets rather than feeding through.

**Real involute gears.** 20° pressure angle, 0.10 mm backlash, root fillets —
replacing the polygonal approximation. Pinion audits at 52.4 mm OD vs 52.5
theoretical.

**Adjustable motor mount.** Separate 62 mm plate with ±2 mm vertical slots so
gear centre distance is set at assembly, not trusted to theory. Pinion hub is
now a slit clamp with an M3 through-bolt, not a set screw into bare PLA.

**Hub Ø26 → Ø23.** Flap-to-hub radial clearance 0.30 → 1.80 mm.

**Coupon set expanded** (`parts/coupons_v6.3mf`): 1.0 / 1.2 / 1.4 mm flaps at
production pivot geometry, plus a **pin-fit gauge** — production lobes against
Ø2.30/2.40/2.50/2.60 pockets. Print it, find the hole that spins freely
without rattle, set `PIN_HOLE_D` accordingly, regenerate.

**Firmware note now enforced in checks:** 164.85 µsteps/flap is fractional —
position must use a phase accumulator, never per-move rounding. 85/33 is a
hunting-tooth pair (gcd = 1) so tooth wear distributes evenly.

**Adopted for the test plan:** 50 plain single-colour flaps for the hand-spin
(not 15) — the packed-stack state the shroud is designed around only exists at
full count. The shroud itself is now understood as a **profile around a motor
keep-out**, not a radius; `SHROUD_R` remains unset until the 50-flap drum
exists.
