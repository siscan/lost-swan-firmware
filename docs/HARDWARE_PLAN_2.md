# HARDWARE PLAN — LOST Swan split-flap display

Standalone reference for sourcing/BOM work. Written to be read cold.

**Every line is tagged:**

* **LOCKED** — decided by design or measurement; safe to spec and buy.
* **OPEN** — still being worked. Do not treat as final; flag choices back
rather than assuming.

Last updated 2026-08-25 (rev 6). Changelog at §11.
**Procurement state lives in `PROCUREMENT_STATUS.md`** — read that first if you
are picking this up cold. Ordering detail and
per-item sourcing live in `claude/BOM.md`.

---

## 1. What the machine is (context for quantities)

Wall-mounted split-flap display, **5 columns** (3 minutes + 2 seconds),
**50 flaps per column**, 80 mm column pitch. Each column is one drum:
two printed spool halves on a fixed 8 mm shaft, driven by a stepper
through a printed gear pair. Clock ~99 % of the time; also a 108-minute
countdown with a hieroglyph reveal, plus a 5-glyph message mode.

**New in rev 3 — the show spin.** A deliberate high-speed blur mode where the
flaps are *not* expected to settle, decelerating into the settle regime only
for the final digit. This is now a headline behaviour and it drives the motor,
rail-voltage, shroud and firmware-architecture decisions below. **LOCKED as an
intent; target speed OPEN** (~400 flaps/s is the electrical ceiling; see §3).

Quantities below derive from **5 columns**. Spares included where the part
is cheap or historically annoying to re-order — see §7 for which spares
actually earn their place.

---

## 2. Already in hand (do not re-buy)

|Item|Qty|Note|
|-|-|-|
|ESP32-C5-DevKitC-1-N8R8|1|**LOCKED** as primary controller. Chip revision unverified until bench-up.|
|Raspberry Pi Pico 2 W|1–2|**LOCKED as shelf spare.** Not used in this build.|
|USB-C PD supplies, multi-hundred-watt|several|**Now the power source.** See §5.|
|NSK 608ZZ bearings|8|⚠️ **NOT USABLE HERE.** 608 = 8×22×7. This build needs **688ZZ = 8×16×5**. A 22 mm OD does not fit the Ø23 hub and would resurrect a flap-clearance problem we already fixed. Keep for other projects.|

---

## 3. Motion — LOCKED unless noted

|Item|Qty|Spec|Notes|
|-|-|-|-|
|NEMA 17 stepper|**5** ✅ in hand|**LDO 42STH48-2504AH(S37)**, 1.5 mH, 2.5 A, 1.2 Ω, 55 N·cm, 48 mm body, Ø5 × 37 mm, JST-XH fitted, Class H|**LOCKED.** Chosen for **low inductance**, not torque. Sourced from **West3D (Beaverton OR, local pickup)** — the StepperOnline 17HS19-2504S-H-V6 (1.6 mH) was the earlier pick but **ships from China only**, as does every StepperOnline part in the 1.6–1.7 mH band. Electrically a wash, so the speed budget is unchanged. ⚠ **Shaft flat unverified** — see §8.3.|
|TMC2209 driver module|5 (+1 = 6)|**standalone mode**, MS1+MS2 high → 1/16 microstep|**LOCKED, in hand: FYSETC TMC2209** from West3D, one batch (BTT was out). Same chip, so nothing here changes. ⚠ **Sense resistor unverified** — research says 110 mΩ but FYSETC documents thinnest. **Set Vref by measurement on driver #1, then replicate**; do not trust `Vref = I_rms × 1.41` blind across the vendor change.|
|688ZZ bearing|10 (buy 20)|8×16×5, shielded|Two per drum. Sits inside the hub on a fixed shaft.|
|8 mm smooth steel rod|5 × 200 mm|goBILDA 2100-0008-0200, precision-ground stainless|**LOCKED** — bought cut to length, no sawing or deburring.|
|Printed pinion, 33 T module 1.5|5|on motor shaft|Printed part. Fixing method still **OPEN** (§8.3), but the 38 mm D-cut shaft keeps every option available.|
|Drum gear, 85 T module 1.5|—|cut into the spool disc rim|Printed as part of the spool.|

**Why a low-inductance motor and not a bigger one (LOCKED reasoning, worth not
re-litigating):** stepper top speed is set by how fast current rises in the
winding — supply voltage over inductance — *not* by holding torque. Inductance
tracks turns count, so longer, higher-torque NEMA 17s are measurably **worse**
at speed while adding rotor inertia. A 60 mm 65 N·cm "upgrade" would be a
downgrade here. Tell: StepperOnline publishes torque curves for this line only
at 24 V, never 12 V.

**Gear/step facts (LOCKED, needed by firmware and by anyone sanity-checking motor choice):**

* Ratio motor:drum = 85 / 33 ≈ 2.576 : 1
* 1/16 microstep → 3200 µsteps per motor revolution
* **µsteps per flap = 3200 × 85 / (33 × 50) = 5440/33 ≈ 164.85** (non-integer)
* Standalone mode cannot do full or half step; 1/8 is the coarsest available
  and 1/16 is what MS1+MS2 high selects.

**Current setting (LOCKED as a starting point):** target **1.2 A RMS →
Vref ≈ 1.69 V**. The motor is rated 2.5 A but the TMC2209's continuous limit
is 2.0 A, and inductance is a property of the winding — you keep the full
speed benefit at reduced current. 1.2 A still yields ~26 N·cm, roughly 13×
demand. Standstill current reduction stays enabled.

**Step rates — §3's old figure was for the clock, not the show spin:**

|Mode|flaps/s|per column|aggregate|
|-|-|-|-|
|Clock / countdown|~2|~330 steps/s|~1.6 k steps/s|
|Old "alarm spin" figure|25|4.1 k steps/s|~21 k steps/s|
|**Show spin**|**~400**|**~66 k steps/s**|**~330 k steps/s**|

⚠️ **This forecloses software step generation.** 330 k steps/s aggregate is not
reachable from a timer ISR. Firmware needs **hardware pulse generation** —
LEDC, MCPWM or RMT on the C5, one channel per column — with the phase
accumulator demoted to flap-boundary bookkeeping rather than per-step timing.
**Settle this before fixing the GPIO map (§8.7)**, since those peripherals
constrain which pins can carry STEP.

---

## 4. Homing / sensing — LOCKED unless noted

|Item|Qty|Spec|Notes|
|-|-|-|-|
|Hall switch, **A1121LUA-T**|5 (+1 = 6)|unipolar digital, open-drain, TO-92, **3.0–24 V**|Runs at **3V3**, pull-up to **3V3**. 95 G operate point.|
|Neodymium magnet|5 (+ spares)|**Ø6 × 3 mm metric**, axially magnetised, N35–N48|One per drum, in a Ø6.2 × 3.0 pocket at R52 in the idler-side disc.|

**Changed in rev 2: the A3144 is gone.** Allegro retired the A314x family. It
is not stocked at Digi-Key or Mouser, and every `A3144` on Amazon or eBay is
unbranded third-party die with no lot traceability and no guarantee it is even
unipolar. The A1121 is a same-outline, same-pinout replacement whose 3.0 V
floor **deletes the 5 V sensor rail entirely** — the ESP32-C5 pin cannot see
over 3.3 V by construction rather than by pull-up discipline.

⚠️ **Buy metric magnets.** The common US disc is 1/4″ × 1/8″ = 6.35 × 3.18 mm
and will not enter a Ø6.2 pocket. Insist on **axially magnetised**; diametric
6×3 discs are also sold and are useless here.

**OPEN:** sensor bracket design and mounting position; which Hall edge is used
for homing; magnet polarity facing the sensor (the A1121 is unipolar too —
bench-test one magnet/sensor pair and mark the working face **before** gluing
ten).

---

## 5. Power — 20 V USB-C PD — LOCKED unless noted

|Item|Qty|Spec|Notes|
|-|-|-|-|
|USB-C PD trigger board|1|**20 V fixed**, ≥5 A|CentyLab RotoPD (AP33772S). Mount at the enclosure entry so its own receptacle is the panel connector.|
|USB-C cable|1|**100 W, 5 A e-marked**|A plain 3 A cable caps you at 60 W regardless of supply.|
|Shunt regulator|1|**Pololu #3774**, 26.4 V trip, 4 Ω, 9 W|The regen strategy. ⚠️ Pololu lists these as rationed.|
|TVS diode|1 (buy 3)|**SMBJ22A**|Transient catcher only — see §9.6.|
|NTC inrush limiter|1 (buy 3)|~5 Ω, 5 A|In series with the bulk cap bank.|
|Buck converter 20→5 V|2|**R-78B5.0-2.0**, 6.5–32 V in, 2 A|12 V of input headroom on this rail. Second is the spare.|
|Electrolytic|10 × 100 µF **50 V**|EEU-FR1H101|One per driver VM, legs bridging **directly** to VM/GND.|
|Bulk electrolytic|1 × 2200 µF **50 V**|EEU-FC1H222|At the rail input. FR doesn't go past 1000 µF at 50 V, hence FC.|
|Ceramic|10 × 0.1 µF 50 V|X7R|In parallel with each 100 µF.|
|Inline fuse + holder|1|**7.5 A** blade|Backstop; the PD source's own OCP is faster and more reliable.|
|Rocker switch|1|12 V+ rated|**Downstream** of the trigger board — switching upstream forces a full PD renegotiation every cycle.|
|Schottky diode|2 (buy 10)|BAT54 / BAT85|EN interlock, see below.|

**Why 20 V and not 24 V (LOCKED).** There is **no 24 V in USB-C PD.** Fixed
PDOs are 5/9/15/20 V (base) and 28/36/48 V (EPR 3.1). 24 V exists only via EPR
AVS, needing a ≥140 W EPR charger *and* an EPR-marked cable. 28 V is above the
TMC2209's 28 V working ceiling before any regen at all. 20 V is also the better
rail: **8 V of headroom to the ceiling instead of 4 V**, which is exactly what
the show-spin decel eats into, and it is the rail the $11 fixed shunt regulator
is built for. Cost: ~17 % of top speed versus 24 V, which is irrelevant.

**Fallback if PD proves troublesome:** Mean Well **GST90A24-P1M**, $29.50,
Digi-Key 7703720, 24 V / 3.75 A / 90 W with a 5.5 × 2.5 barrel plug. Note the
fixed shunt regulator no longer fits a 24 V rail; use the adjustable Pololu
#3779 set to ~27 V.

**EN interlock (LOCKED, ~50 ¢ of parts).** The trigger board's **Power Good is
open-drain, low = good**; the TMC2209 **EN is active-low**. They compose
directly. Put a 10 kΩ pulldown on the ganged EN node and diode-OR two "disable"
signals into it — one Schottky from PG, one from the ESP32 GPIO, anodes at the
sources, cathodes at EN. Either high forces all five drivers off; both low
enables. Firmware keeps full control and can no longer enable into a dead rail.

Note the 5 V PD negotiation phase is inherently safe: the Recom buck needs
6.5 V in, so the logic rail is simply down and the drivers have no VIO.

**Deceleration is an electrical parameter, not firmware taste.** Each drum
stores roughly **0.8 J at show speed** (~8 rev/s), ~4 J across five.
Capacitance cannot absorb that — 2200 µF going 20 → 28 V takes only 0.42 J.
Windage can: 50 flaps at ~3.3 m/s tip speed burn ~1 W per drum, so a decel
**ramped over ~1 second** bleeds nearly all of it as air drag for free. Slam it
in 100 ms and the remainder goes into the rail, which is what the shunt
regulator is for.

**Three PD behaviours to design around:**

1. **Cold start comes up at 5 V.** VBUS sits at 5 V until negotiation completes,
   tens to a few hundred ms after attach. The EN interlock covers this, and also
   covers PD Hard Reset, where VBUS drops to 0 and returns at 5 V mid-operation.
2. **Re-home on rail recovery.** A Hard Reset loses the move in flight. Hall
   homing is the clean recovery path.
3. **Regen never reaches the charger.** A proper PD sink opens its pass FET on
   reverse current, so the energy stays trapped on *your* local rail and pumps
   *your* capacitors. The charger is not the casualty; the drivers are. That is
   why the shunt regulator sits on your side of the trigger board.

**Negotiate the full 100 W contract** even though the machine draws ~60 W.
Peaks that exceed the contract trip source OCP, which is a hard dropout to
0 V — not a graceful renegotiation.

---

## 6. Audio — LOCKED

|Item|Qty|Spec|Notes|
|-|-|-|-|
|MAX98357A I2S amp|1|Adafruit 3006, 3.2 W|Gain pin left at default (9 dB). Adafruit's board guarantees that default; clone gain resistors vary.|
|Speaker|1|Adafruit 3968, 40 mm, 4 Ω, 5 W|5 W rating gives headroom over the amp. ⚠️ Not the CUI GF0401M — that is 8 Ω / 0.1 W, beeper grade.|

Decision rationale (in case it's questioned): the Swan timer's warning beeps
and system-failure alarm are core to the object, and browser-side audio plays
in the wrong room. Cost is ~3 GPIOs and a few dollars.

---

## 7. Wiring / assembly hardware — LOCKED unless noted

|Item|Qty|Notes|
|-|-|-|
|JST-XH connector kit, 2.54 mm|1 box|Per module: one 4-pin (motor) + one 3-pin (Hall). **The V6 motors ship with XH fitted**, so this mainly covers Hall connectors and board headers. **Pin order OPEN** — fix it when the harness is crimped and record it.|
|Ratcheting crimp tool|1|iCrimp SN-2549, AWG 28–18. **Buy this even if the kit includes pliers** — kit pliers make intermittent crimps, the exact failure you don't want sealed in a drum.|
|Silicone wire, 22 AWG|5 colours|Motor runs.|
|Silicone wire, 24 AWG|1 kit|**Hall runs.** XH contacts are rated 22–28 AWG and 22 AWG silicone insulation often won't fit the wings. The Halls draw milliamps. Bonus: makes the two harness types physically distinguishable.|
|Heatshrink assortment|1||
|Perfboard 7×9 cm + screw terminals|1 + ~10|The "motherboard" unless/until a PCB happens. **5.08 mm terminals, not 3.5 mm** — 5.08 is exactly 2× the 0.1″ grid and drops straight in.|
|Resistor assortment, 1/4 W|1 kit|Hall pull-ups, EN pulldown, rail-sense divider.|
|Dupont jumper stock|—|Bring-up only.|
|M5 threaded rod|**4** × 1 m ⚠ **not yet bought**|Tie rods, cut ~520 mm × 4. **Four, not two or three** — each 1 m rod yields exactly *one* 520 mm piece plus a 480 mm unusable offcut. Total linear length is the wrong way to count this. **Buy local** (Lowe's / fastener counter): mail order quotes ~$44 shipping on ~$13 of rod.|
|M5 nuts + washers|~100 / ~100|Include ~8 nyloc. Bulk bags; do not buy these at Home Depot.|
|M3 screw assortment|1 box|6–20 mm, plus nuts/washers.|

**Which spares actually earn their place (LOCKED).** Not the stepper: it runs at
~2 N·cm of 55, brushless, barely-loaded bearings, no realistic wear-out path,
and it is the most expensive spare in the plan. **Keep the TMC2209 and Hall
spares** — the driver is the part that actually dies (hot-unplug a motor or
short a lead and it goes while the motor shrugs it off), and the Halls get glued
into position and are miserable to swap. Bearings and magnets come in packs
where spares are free anyway.

**GPIO budget (LOCKED as a count, map OPEN):** 5× STEP + 5× DIR + 1× EN
(ganged, active-low) + 5× Hall + 3× I2S + 1× PD Power Good + 1× rail sense +
status LED ≈ **22 pins**. Fits the C5 with room. ⚠️ The STEP pins are now
constrained by which LEDC/MCPWM/RMT channels can reach them — see §3.

---

## 8. OPEN items — still being worked, do not finalise without checking back

1. **JST pin order** for the motor and Hall connectors.
2. **Hall sensor bracket** geometry and mounting; trigger edge; magnet polarity
   convention. **M3 heat-set inserts in hand.** ⚠ Mark the magnets' working face
   with an **oil-based paint marker** — Sharpie rubs off Ni-Cu-Ni plating.
3. **Pinion fixing method** to the motor shaft (grub screw vs clamp vs press).
   **M3 brass heat-set inserts are now in hand**, so the printed-boss route is
   unblocked. ⚠ **Still gated on a physical check.** LDO marks round-shaft variants with an `R`
   suffix (the S45R is round), so the S37 *should* be D-cut — but no vendor publishes
   a drawing and LDO's RevA datasheets are scanned images. **Inspect the shaft at
   pickup.** If it is round: file a flat (trivial on Ø5), or use a clamping hub.
4. **Fixed-shaft retention** — how the 8 mm rod is held stationary in the module
   walls. Not yet designed.
5. **Drum rotation direction** relative to the sensor. Firmware carries a
   per-column `invert_dir` flag.
6. **Show-spin target speed.** ~400 flaps/s is the electrical ceiling at 20 V.
   What actually looks right, and what the printed parts tolerate, is a
   bench question.
7. **GPIO pin map** — now gated on the step-generation peripheral choice (§3).
8. **Step generation architecture** — LEDC vs MCPWM vs RMT. New in rev 3.
9. **Faceplate** — 2 mm aluminium, laser cut (SendCutSend-class vendor).
   Deferred until the window trim is final; DXF not exported yet.
10. **Enclosure hardware** — French cleat, felt/foam liner, fasteners. Now also
    gated on the shroud clearance question in §10.
11. **Filament top-up** — **PETG**, ~1.5 kg black, ~1 kg white, ~0.75 kg red.
    **Resolved: Ambrosia PETG from West3D**, $20.99/kg, black/white/red all in stock
    on the same pickup — one brand, one shrinkage figure.
    Material changed from PLA: snap-fit flap pivots and module-1.5 gear teeth
    both want impact toughness where PLA goes brittle, and PETG's ~0.3 % shrink
    prints open-air. **Order red first** — it is the constrained colour at every
    vendor and it picks the brand for all three, and one brand means one
    shrinkage figure. ⚠️ **Re-run one fit-check print before committing 3+ kg**;
    §12's corrections were measured on PLA.

---

## 9. Gotchas worth carrying into any BOM

1. **688ZZ, not 608ZZ.** Same 8 mm bore, wrong body. The 16 mm OD is what
   allows the slim Ø23 hub, which is what clears the flaps.
2. **Same-vendor, same-batch drivers.** Five modules from five sellers means
   five Vref formulas.
3. **Hall polarity.** The A1121 is unipolar; a flipped magnet reads as a dead
   sensor. Bench-test before assembly. Survives the A3144 → A1121 change.
4. **Standstill current reduction stays enabled** (TMC default) — it is the
   holding-torque plan, not an efficiency tweak.
5. **Ten bearings, not eight.** Two per drum × five drums.
6. **A TVS cannot clamp this rail.** SMBJ clamping voltage is a consistent
   **1.62× the standoff**: SMBJ20A stands off 20 V and clamps at 32.4 V, above
   the driver ceiling — and on a 20 V PD rail SMBJ20A is unusable anyway, since
   a legitimate 20 V PDO can sit near 21 V. Fit an SMBJ22A as a **transient
   catcher** for connector-yank and driver-disable events. It is not the regen
   strategy; the ramped decel and the shunt regulator are.
7. **Do not trust the reference design's protection.** The SparkFun PD Stepper
   (ESP32-S3 + TMC2209 + 20 V PD, the same architecture) ships with **one
   100 µF electrolytic, one 22 µF MLCC and a README warning** — no rail TVS, no
   clamp, no inrush limiter, no fuse. Its own docs say *"stepper motors should
   never be back driven at a high speed as they can act as a generator and cause
   damage."* That warning **is** its strategy. It survives in the wild because
   the dominant use is blinds and turntables — low inertia, held rather than
   decelerated. That is the opposite of a show spin.
8. **Gate EN in hardware.** The same reference board gates EN in firmware only,
   and its shipping web-server example asserts EN enabled in `setup()` *before*
   Power Good is ever read. See §5.

---

## 10. Mechanical status (context only — not a purchase list)

* Spool halves and flap geometry are printed-and-measured, currently in a
  fit-correction cycle. Corrections were measured on **PLA** prints; the
  material is now PETG, so one fit-check re-run is needed.
* Character ring is frozen at 50 slots; Column 5 production flaps generated.
* Shroud, module walls, enclosure and faceplate are not yet designed — all
  gated on a drum spin test.

**⚠️ The show spin is a shroud constraint, and it is free to design in now.**
At rest the flaps hang inward and the swept diameter is the disc. Above about
**1 g of centrifugal load (~120 flaps/s)** they stand out radially and the
swept diameter jumps to full flap extension. **Size the shroud for flaps fully
extended, not stacked.** Free today, a full reprint later.

**Two more consequences of the show spin:**

* **Module depth.** The V6 is a **48 mm body**, 10 mm deeper than the 38 mm
  previously assumed. Design the walls for 48 mm.
* **Balance.** Spin-up and spin-down both pass through a regime where some
  flaps are out and some are stacked, so the rotor is unbalanced — roughly
  0.3 N of rotating force per drum at 8 rev/s. Stiffen the mount, and don't
  linger in the transition.
* **Settle dwell.** After decelerating below ~1 g the flaps drop back in at
  semi-random times. Step position is never lost (flaps are captive), but the
  *picture* needs a few hundred ms of dwell before the final move to the digit.

---

## 11. Changelog

**rev 6 — 2026-08-25.** Procurement executed; see `PROCUREMENT_STATUS.md`.
Driver vendor **BTT → FYSETC** (same TMC2209 chip; sense resistor now unverified,
so Vref must be set by measurement). Motors, drivers and **M3 brass heat-set
inserts** collected from West3D, unblocking the printed-boss route for §8.3/§8.4.
8 mm shaft and 688ZZ bearings moved to Amazon. Fasteners covered by a VIGRUE
M2–M5 kit on hand. Outstanding: M5 threaded rod ×4, a 5 A e-marked USB-C cable
(missed on the Digi-Key order), M5 nyloc, red PETG.

**rev 5 — 2026-08-22.** M5 tie-rod count corrected **3 → 4** (§7): each 1 m length
yields one 520 mm piece, so linear-length arithmetic understated it. Rod and bulk
fasteners moved to local purchase after a mail-order quote of $43.71 shipping on
$12.63 of rod.

**rev 4 — 2026-08-22.** Motor changed to **LDO 42STH48-2504AH(S37)** sourced from
West3D (local pickup) after discovering the StepperOnline V6 ships from China only
(§3). Drivers and filament moved to the same West3D pickup. Pinion fixing (§8.3) now
gated on a physical shaft inspection. Filament brand resolved (§8.11).

**rev 3 — 2026-08-22.** Show spin promoted to a headline behaviour (§1).
Stepper changed to 17HS19-2504S-H-V6 and the low-inductance reasoning recorded
(§3). Step-rate table added; software step generation ruled out (§3).
Power architecture moved from a 12 V brick to a **20 V USB-C PD rail**, with
shunt regulator, TVS, NTC and a hardware EN interlock (§5). Caps to 50 V.
Stepper spare dropped; driver and Hall spares kept (§7). Shroud clearance,
module depth, balance and settle dwell added (§10). Gotchas 6–8 added.

**rev 2 — 2026-08-22.** A3144 → A1121LUA-T; 5 V sensor rail deleted (§4).
M5 rod 2 → 3 (§7). Magnets specified metric and axial (§4). Caps 25 → 35 V,
ceramics added (superseded by rev 3). 8 mm shaft and PSU resolved.
Filament changed PLA → PETG (§8.11).

**rev 1 — 2026-08-22.** Original.
