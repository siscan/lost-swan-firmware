# Mechanical → Firmware handoff: drive architecture changed (2026-09-06)

Paste this into the firmware overseer chat.

**Context since your last update.** The v3 spool joint is bench-confirmed
(span 66.53 at four angles, single-flap release, README §25). Then, with
50 flaps loaded, the 36T pinion and motor could not be placed anywhere
around the drum without sitting in the flaps — a mesh sweep proved no
collision-free angle exists (§26, docs/DRIVE_STUDY.md). Four alternatives
were studied with numbers (A: motor inside the drum; B: outboard ring gear;
C: narrow cards; E: rim lip into the inter-column gap). Nico chose A on
"most reliable machine". It is gated on a bench test with a printed
stand-in axle before any purchase (§28b). Mechanical session 03 is
producing the parts now. The repo is github.com/siscan/swan-mechanical
(private) — BUILD_README §26–§28b and docs/DRIVE_STUDY.md are the record.

**What changed.** The 85T/36T rim-gear drive is dead: with 50 flaps loaded,
the pinion collides with the card edges at every angular position (mesh
sweep, BUILD_README §26, docs/DRIVE_STUDY.md). Decision (§28): the NEMA 17
now sits INSIDE the drum, stationary, and drives the drum directly, 1:1.

**New constants — replace 85/36 everywhere.**
| | was | now |
|---|---|---|
| reduction | 85/36 = 2.361 | **1 : 1** |
| µsteps/flap (1/16) | 1360/9 = 151.111 | **64 exact** |
| motor steps per drum rev | 7555.5 | **3200 exact** |
| hall-to-hall discriminator | 7555–7556 | **3200** |
| motor speed at 400 flaps/s | 18.9 rev/s | **8.0 rev/s (480 rpm)** |
| motor speed at 25 flaps/s | 1.18 rev/s | 0.5 rev/s |
| reflected torque demand | ~2 N·cm + windage | **~9.1 N·cm** (drum imbalance 3.92 N·cm static) |
| available at 1.2 A RMS | ~30 N·cm | ~30 N·cm → 3.3× margin |
| detent torque vs static hold | n/a | 2.2 vs 3.92 → **coils must stay energised** |
| per-column invert_dir | kept | kept |

The phase accumulator becomes unnecessary (integer steps/flap) — keep it
if it costs nothing, but 64 is exact and the ring is 50 × 64 = 3200.

**Decel / regen.** Drum inertia and energy are unchanged (~2.1 J/drum at
show speed), but the motor now turns 2.36× slower for the same flap rate,
so back-EMF at show speed is 2.36× lower. Re-check the 2 s ramp floor and
the shunt duty; expect both to relax. Standstill current reduction
remains the holding plan.

**HEAT — this is the one new hard requirement.** The motor is sealed
inside a PLA drum (softens 55–60 °C) and the clock idles 99 % of the day.
Static hold needs only ~13 % of 1.2 A (0.16 A). But the TMC2209 in
STANDALONE mode can only reduce standstill current to ~50 % of run
current. At 50 % of 1.2 A the sealed motor lands ~35–48 °C depending on
the loss model — at the 45 °C flag. Two resolutions, pick one:
1. **Run current ≤ 0.7 A** (still ~2× torque margin at 1:1). Hold is then
   ≤ 0.35 A → trivial heat. Set Vref accordingly (measure, per §3 note).
2. **Move to UART mode** (PDN_UART to the C5; TMC2209 has 2 address bits →
   two buses for five drivers, or one bus with the fifth on its own) and
   set IHOLD ≈ 15 %, IRUN as needed. Also unlocks StallGuard and CoolStep.
Either way, the hold current must be a firmware/hardware contract, not a
driver default. Mechanical will give the motor a conductive path (the
steel/aluminium support tube) but is not relying on it.

**Homing.** Hall at R52 on the idler disc, unchanged. One pulse per drum
revolution = one per 3200 µsteps.

**Wiring.** Motor leads exit through the Ø6 bore of the support tube at
the idler wall. Fixed motor, no slip ring. Cable length per column grows
by ~60 mm.

**What mechanical needs from firmware, for the stand-in test (§28b gate 3):**
a bench build that drives ONE real column on a real TMC2209 with:
- run current ≤ 0.7 A RMS (state the Vref you set and how you measured it),
  standstill reduction on;
- 64 µsteps/flap, homing on the hall, one flap per tick at clock cadence
  for a one-hour soak (heat test — Nico feels the motor case at the end);
- a slow continuous spin mode (≤ 1 drum rev/s) for runout and wire-routing
  checks — NO show spin on the PLA stand-in axle;
- direction selectable (invert_dir), since the drum's sense is set by the
  descending ring and the motor now faces the other way relative to the
  old bridge.
Decide standalone-at-0.7 A vs UART/IHOLD for production and say which;
the stand-in test can run standalone either way.

Firmware does NOT need the shaft cut length, coupling type or motor axial
position — those are mechanical outputs of session 03 and do not affect
any constant above.