# HARDWARE PLAN — LOST Swan split-flap display

Standalone reference for sourcing/BOM work. Written to be read cold.

**Every line is tagged:**

* **LOCKED** — decided by design or measurement; safe to spec and buy.
* **OPEN** — still being worked. Do not treat as final; flag choices back
rather than assuming.

Last updated 2026-08-22.

\---

## 1\. What the machine is (context for quantities)

Wall-mounted split-flap display, **5 columns** (3 minutes + 2 seconds),
**50 flaps per column**, 80 mm column pitch. Each column is one drum:
two printed spool halves on a fixed 8 mm shaft, driven by a stepper
through a printed gear pair. Clock \~99 % of the time; also a 108-minute
countdown with a hieroglyph reveal, plus a 5-glyph message mode.

Quantities below derive from **5 columns**. Spares included where the part
is cheap or historically annoying to re-order.

\---

## 2\. Already in hand (do not re-buy)

|Item|Qty|Note|
|-|-|-|
|ESP32-C5-DevKitC-1-N8R8|1|**LOCKED** as primary controller. Chip revision unverified until bench-up.|
|Raspberry Pi Pico 2 W|1–2|**LOCKED as shelf spare.** Not used in this build (PIO step-gen was the alternative architecture; ESP32 won on ecosystem).|
|NSK 608ZZ bearings|8|⚠️ **NOT USABLE HERE.** 608 = 8×22×7. This build needs **688ZZ = 8×16×5**. A 22 mm OD does not fit the Ø23 hub and would resurrect a flap-clearance problem we already fixed. Keep for other projects.|

\---

## 3\. Motion — LOCKED unless noted

|Item|Qty|Spec|Notes|
|-|-|-|-|
|NEMA 17 stepper|5 (+1 spare = 6)|200 full-steps/rev (1.8°), \~40 N·cm, 1.5–1.7 A, 5 mm shaft|**Model OPEN** — see §8. Torque demand is tiny (\~2 N·cm reflected); buy for build quality, not torque.|
|TMC2209 driver module|5 (+1 = 6)|**standalone mode**, MS1+MS2 high → 1/16 microstep|**Vendor OPEN** — see §8. Standalone is LOCKED: TMC UART addressing tops out at 4 devices/bus and we have 5.|
|688ZZ bearing|10 (buy 12)|8×16×5, shielded|Two per drum. Sits inside the hub on a fixed shaft.|
|8 mm smooth steel rod|\~1 m total|≥180 mm per column + slop|One fixed (non-rotating) shaft per column. **Purchase status OPEN.**|
|Printed pinion, 33 T module 1.5|5|on motor shaft|Printed part, not purchased. Needs a grub-screw or clamp fixing — **method OPEN**.|
|Drum gear, 85 T module 1.5|—|cut into the spool disc rim|Printed as part of the spool.|

**Gear/step facts (LOCKED, needed by firmware and by anyone sanity-checking motor choice):**

* Ratio motor:drum = 85 / 33 ≈ 2.576 : 1
* 1/16 microstep → 3200 µsteps per motor revolution
* **µsteps per flap = 3200 × 85 / (33 × 50) = 5440/33 ≈ 164.85** (non-integer; firmware uses a phase accumulator)
* Worst case (alarm spin, \~25 flaps/s, all five columns) ≈ 21 k steps/s aggregate

\---

## 4\. Homing / sensing — LOCKED in principle, geometry OPEN

|Item|Qty|Spec|Notes|
|-|-|-|-|
|Hall switch, A3144|5 (+1 = 6)|unipolar digital, TO-92|Runs at **5 V** (part needs 4.5 V min). Output is open-collector → **pull-up to 3V3** so the ESP32 pin never sees 5 V.|
|Neodymium magnet|5 (+ spares = 12)|Ø6 × 3 mm, N35|One per drum, in a Ø6.2 × 3.0 pocket at R52 in the idler-side disc.|

**OPEN:** sensor bracket design and mounting position; which Hall edge is used
for homing; magnet polarity facing the sensor (A3144 is unipolar — bench-test
one magnet/sensor pair and mark the working face **before** gluing ten).

\---

## 5\. Power — LOCKED unless noted

|Item|Qty|Spec|Notes|
|-|-|-|-|
|12 V PSU|1|\~6 A / 72 W|All-five-spinning worst case ≈ 4–5 A. **Form factor OPEN** (external brick preferred over open-frame for a wall box).|
|Buck converter 12→5 V|2|≥3 A|Logic, sensors, audio. Second is the spare.|
|Inline fuse + holder|1|5 A blade|On the 12 V input.|
|Panel barrel jack|1|5.5 × 2.5 mm|Plus matching plug if the PSU lead needs re-terminating.|
|Rocker switch|1|12 V rated||
|Electrolytic caps|6 × 100 µF, 1 × 1000 µF|25 V|One 100 µF across each driver's VM; 1000 µF at the input. Driver on-board caps alone are marginal.|

\---

## 6\. Audio — LOCKED

|Item|Qty|Spec|Notes|
|-|-|-|-|
|MAX98357A I2S amp|1|3 W mono|Gain pin left at default (9 dB).|
|Speaker|1|40 mm full-range, 4 Ω 3 W|Mounts to the back panel; the flap window leaks plenty of sound, no grille needed.|

Decision rationale (in case it's questioned): the Swan timer's warning beeps
and system-failure alarm are core to the object, and browser-side audio plays
in the wrong room. Cost is \~3 GPIOs and a few dollars.

\---

## 7\. Wiring / assembly hardware — LOCKED unless noted

|Item|Qty|Notes|
|-|-|-|
|JST-XH connector kit, 2.54 mm|1 box|Per module: one 4-pin (motor) + one 3-pin (Hall). **Pin order OPEN** — fix it when the harness is crimped and record it.|
|Silicone wire, 22 AWG|5 colours|Motors usually ship with \~1 m leads; verify the listing.|
|Heatshrink assortment|1||
|Perfboard 7×9 cm + screw terminals|1 + \~10|The "motherboard" unless/until a PCB happens.|
|Dupont jumper stock|—|Bring-up only.|
|M5 threaded rod|2 × 1 m|Tie rods, cut \~520 mm × 4.|
|M5 nuts + washers|\~30 / \~20|Include \~8 nyloc.|
|M3 screw assortment|1 box|6–20 mm, plus nuts/washers: motor mounts, pinion fixing, sensor brackets, electronics.|

**GPIO budget (LOCKED as a count, map OPEN):** 5× STEP + 5× DIR + 1× EN
(ganged, active-low) + 5× Hall + 3× I2S + status LED ≈ **20 pins**. Fits the
C5 with room.

\---

## 8\. OPEN items — still being worked, do not finalise without checking back

1. **Stepper model.** 17HS4401-class recommended; nothing ordered.
2. **TMC2209 vendor** (BTT / FYSETC / other). Matters because the **Vref
formula is vendor-specific** and silkscreen orientation varies. Buy all
five from **one vendor, one batch**. Target ≈ 1.1–1.2 A RMS.
3. **JST pin order** for the motor and Hall connectors.
4. **Hall sensor bracket** geometry and mounting; trigger edge; magnet
polarity convention.
5. **Drum rotation direction** relative to the sensor — depends on which side
the motor mounts. Firmware will carry a per-column `invert\_dir` flag.
6. **PSU form factor / exact model.**
7. **Pinion fixing method** to the motor shaft (grub screw vs clamp vs press).
8. **Fixed-shaft retention** — how the 8 mm rod is held stationary in the
module walls. Not yet designed.
9. **GPIO pin map.**
10. **Faceplate** — 2 mm aluminium, laser cut (SendCutSend-class vendor).
Deferred until the window trim is final; DXF not exported yet.
11. **Enclosure hardware** — French cleat, felt/foam liner, fasteners.
12. **Filament top-up** — rough estimate \~1.5 kg black, \~1 kg white, \~0.75 kg
red. Red demand rose sharply when the character ring went glyph-heavy
(columns 4–5 carry \~36 red-bodied flaps each). Treat as an estimate.

\---

## 9\. Gotchas worth carrying into any BOM

1. **688ZZ, not 608ZZ.** Same 8 mm bore, wrong body. The 16 mm OD is what
allows the slim Ø23 hub, which is what clears the flaps.
2. **Same-vendor, same-batch drivers.** Five modules from five sellers means
five Vref formulas.
3. **Hall polarity.** A3144 is unipolar; a flipped magnet reads as a dead
sensor. Bench-test before assembly.
4. **Standstill current reduction stays enabled** (TMC default) — it is the
holding-torque plan, not an efficiency tweak.
5. **Ten bearings, not eight.** Two per drum × five drums.

\---

## 10\. Mechanical status (context only — not a purchase list)

* Spool halves and flap geometry are printed-and-measured, currently in a
fit-correction cycle (pocket depth and pivot engagement increased after
caliper measurements of real prints). None of it changes the electrical
BOM or the gear/step numbers above.
* Character ring is frozen at 50 slots; Column 5 production flaps generated.
* Shroud, module walls, enclosure and faceplate are not yet designed — all
gated on a drum spin test. Any hardware they imply is **OPEN**.

