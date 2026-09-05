# PROCUREMENT STATUS — LOST Swan split-flap

Written to be read cold by any chat in this project. Snapshot as of **2026-08-25**.
**Status: procurement COMPLETE.** Nothing left to buy. What remains is verification
before assembly — see §4.
Full sourcing rationale lives in `claude/BOM.md`; design decisions in
`HARDWARE_PLAN_2.md` (now rev 6).

---

## 1. TL;DR

**Procurement is done.** Every line on the BOM is ordered or in hand across seven
vendors plus local pickups. No outstanding purchases.

The build is now gated on **verification** (§4) and on the **still-open §8 design
items** in `HARDWARE_PLAN_2.md` — chiefly the pinion fixing method, which is
waiting on a physical look at the motor shaft.

---

## 2. Ordered / in hand

| Vendor | Status | Contents |
|-|-|-|
| **West3D** (Beaverton, pickup) | ✅ **COLLECTED** | 5 × LDO 42STH48-2504AH(S37) steppers · 6 × **FYSETC** TMC2209 · M3 brass heat-set inserts |
| **Digi-Key** | ✅ ordered, $102.36 | 17 lines — A1121 Halls ×6, Recom buck ×2, 50 V caps, SMBJ22A TVS ×3, terminals, rocker, 1N5817 ×10, Adafruit 3006 amp, Adafruit 3968 speaker, genuine JST-XH set, ceramics, NTC ×3 |
| **Pololu** | ✅ ordered, $11.49 | #3774 shunt regulator ×1 |
| **Tindie / CentyLab** | ✅ ordered #598468, $23.00 | RotoPD USB-C PD trigger ×1 |
| **totalElement** | ✅ ordered, $24.79 | 120 × Ø6×3 mm N42 discs — **axial confirmed**, ±0.004″ |
| **Amazon** | ✅ ordered | uxcell 8 mm × 200 mm linear rod · 688ZZ bearings · IWISS SN-2549 crimper · silicone wire 22 & 24 AWG · heatshrink · perfboard · fuse holders · Dupont · resistor kit |
| **Home Depot** | ✅ collected | 30-min epoxy · Loctite 242/243 · hacksaw + blades · cutting oil · files · isopropyl · zip ties |
| **Local (rod & fasteners)** | ✅ collected | M5-0.8 threaded rod · M5 nyloc nuts |
| **Filament** | ✅ in hand | PETG — black / white / red |
| **8 mm shafts** | ✅ in hand | uxcell 8 × 200 mm linear rod, HRC50, Ø tol −0.02 mm |

**Already on hand:** ESP32-C5-DevKitC-1-N8R8 · Pico 2 W (shelf spare) ·
**VIGRUE 1225 pc M2–M5 kit** (60 × M5 nuts, 95 × M5 washers, M3–M5 SHCS 8–20 mm) ·
assorted wire, Dupont, perfboard · filament stock (Overture + others).

---

## 3. Changes from the plan — record these

1. **Driver vendor: BTT → FYSETC.** West3D was out of BTT TMC2209. **Same chip**, so
   nothing in §3 changes — but the 110 mΩ sense resistor is now **unverified**.
   ⚠ **Set Vref by measurement on driver #1, then replicate.** Do not trust
   `Vref = Irms × 1.41` blind across a vendor change.
2. **Motor: LDO 42STH48-2504AH(S37)** — 1.5 mH, 2.5 A, 55 N·cm, 48 mm, Ø5 × 37 mm,
   JST-XH fitted, Class H. Replaces StepperOnline 17HS19-2504S-H-V6, which ships
   from **China only**.
3. **M3 brass heat-set inserts acquired** — closes the hardware gap behind §8.3
   (pinion fixing) and §8.4 (Hall bracket).
4. **8 mm shaft: goBILDA → Amazon uxcell**, 8 × 200 mm, HRC50, Ø tolerance
   **−0.02 mm**. goBILDA quoted $12 shipping on $24.95. ⚠ Length tolerance is
   **+1~2 mm and the five will not match each other**.
5. **688ZZ bearings: Harfington → Amazon.** Harfington's free-ship threshold is $30
   against a $9.65 order.
6. **M5 tie rod: 4 lengths, not 2 or 3.** Each 1 m rod yields exactly one 520 mm
   piece plus an unusable 480 mm offcut. Linear-length arithmetic was wrong twice.
7. **Fasteners covered by the VIGRUE kit on hand** — M5 nuts, M5 washers, M3–M5
   screws all in stock. Kit is 12.9 black oxide, not stainless; fine indoors.

---

## 4. ⚠ Verify before assembly — nothing left to buy

These are the open loops that could still cost a rebuild. None require a purchase.

1. **Motor shaft: does the S37 have a D-cut flat?** LDO marks round variants with an
   `R` suffix so it should — but no vendor publishes a drawing. **Gates §8.3
   (pinion fixing).** If round: file a flat, or use a clamping hub. M3 heat-set
   inserts are on hand either way.
2. **M5 rod: confirm you have enough for FOUR continuous 520 mm pieces.** Each 1 m
   length yields exactly **one** 520 mm piece plus an unusable 480 mm offcut —
   linear-length arithmetic understated this twice during planning. ≥1060 mm stock
   yields two pieces.
   ⚠ **Cut one, dry-fit the actual stack, then cut the other three.** 520 mm is the
   plan's figure and has never been checked against built module geometry.
3. **Measure all five 8 mm shafts.** Length tolerance is **+1~2 mm and they will not
   match each other.** Either design §8.8 (shaft retention) to tolerate it, or face
   them to a common length.
4. **FYSETC drivers: all six the same board revision?** V3.0 and V4.0 both ship.
   And **are heatsinks in the box?** BTT's included them; FYSETC's may not.
5. **Set Vref by measurement on driver #1, then replicate.** The vendor changed, so
   the 110 mΩ assumption behind `Vref = I_rms × 1.41` is unverified.
   Target **1.2 A RMS**.
6. **USB-C cable — RESOLVED.** ✅ Cables on hand are specced 120 W and 240 W, both of
   which require a 5 A e-marker (240 W is EPR-rated, 50 V/5 A). Nothing to buy or
   check. Optional: a ~$15 inline USB-C PD power meter is still worth having to
   confirm the rail actually negotiated 20 V and to watch current during spin-up —
   that draw is otherwise invisible until it trips something.
7. **Filament: confirm black / white / red are ONE brand.** §10's fit corrections
   assume a single shrinkage figure. Dry the PETG before the production run —
   wet PETG prints with weak layer adhesion, which is the exact failure mode
   (snapped pivots, sheared gear teeth) the PLA→PETG change was meant to prevent.
8. **Bench-test one magnet/sensor pair and mark the working face with an oil-based
   paint marker** before gluing any. The A1121 is unipolar; a flipped magnet reads
   as a dead sensor. Sharpie rubs off Ni-Cu-Ni plating.

## 5. Reference — unboxing notes

- **Motor shaft: does the S37 have a D-cut flat?** LDO marks round variants with an
  `R` suffix, so it should — but no vendor publishes a drawing. Gates §8.3.
- **Measure all five 8 mm rods.** +1~2 mm length tolerance means they won't match;
  either design §8.8 to not care, or face them to a common length.
- **FYSETC drivers: all six the same board revision?** V3.0 and V4.0 both ship.
- **Heatsinks present** with the FYSETC boards? BTT's included them.
- **1N5817** — ordered the STMicro `497-4547-1-ND`; the onsemi variant was out.
- **Bench-test one magnet/sensor pair and mark the working face with an
  oil-based paint marker** before gluing any. Sharpie rubs off Ni-Cu-Ni plating.

---

## 6. Money

| Vendor | Spend |
|-|-|
| West3D | ~$190 |
| Digi-Key | $102.36 |
| Amazon | ~$143 |
| Tindie | $23.00 |
| totalElement | $24.79 |
| Pololu | $11.49 |
| Home Depot | ~$60 |
| Local (rod, nyloc, filament) | ~$60 |
| **Total** | **≈ $615** |
