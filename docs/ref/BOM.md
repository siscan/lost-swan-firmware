# HARDWARE BOM — LOST Swan split-flap

Quantities include spares. ⏱ = order now, longest/most annoying lead times.
Prices are ballparks for sanity, not quotes.

## 0. Already sorted
| Item | Qty | Note |
|---|---|---|
| ESP32-C5 devkit | 1 | **primary controller** — build on current ESP-IDF / latest Arduino core |
| Pi Pico 2 W | 1 | shelf spare / future project (PIO step-gen alt) |
| 8 mm smooth rod | 1 m | hardware store — drum test needs ~180 mm of it *now* |

## 1. Motion ⏱
| Item | Qty | Spec | Gotchas |
|---|---|---|---|
| NEMA 17 stepper | 5 | 17HS4401-class: 40 N·cm, 1.5-1.7 A, 5 mm D-shaft, ~38-40 mm body | torque need is tiny (~2 N·cm reflected) — buy for build quality, not torque. Confirm 42.3 mm body vs mount |
| TMC2209 driver module | 6 | BTT or FYSETC, all same vendor + batch | pinout/Vref formula varies by vendor. Run **standalone** (no UART): MS1+MS2 high = 1/16. Only 4 UART addresses exist, we have 5 drivers — standalone dodges it |
| 688ZZ bearing | 12 | 8×16×5 | NOT 608. Cheap in 10-packs |
| Magnet Ø6×3 | 12 | N35 neodymium | pocket is 6.2×3.0. Mark polarity of the first one that works and match the rest |
| Hall sensor A3144 | 6 | TO-92 digital | unipolar: wrong magnet face = no trigger. Test polarity before gluing anything |
| M5 threaded rod | 2×1 m | zinc or SS | tie rods cut ~520 mm ×4 |
| M5 nuts + washers | 30/20 | incl. 8 nyloc | |
| M3 assortment box | 1 | 6-20 mm, nuts, washers | motor mounts, pinion clamps, sensor brackets, electronics |

## 2. Power
| Item | Qty | Spec | Gotchas |
|---|---|---|---|
| 12 V PSU | 1 | 6 A / 72 W — Mean Well GST60A12 brick or LRS-75-12 | brick = simpler + safer in a wood-adjacent wall box. All-5-spinning worst case ≈ 4-5 A |
| Buck 12→5 V | 2 | 3 A (MINI560 class) | logic + sensors + audio. Second is the spare |
| Panel barrel jack | 1 | 5.5×2.5 | + matching plug if PSU lead needs re-terminating |
| Inline fuse + holder | 1 | 5 A blade | on the 12 V input |
| Rocker switch | 1 | 12 V rated | |
| Bulk caps | 6× 100 µF, 1× 1000 µF | 25 V electrolytic | one 100 µF across each driver's VM, 1000 µF at input. Driver modules' onboard caps alone are marginal |

## 3. Audio
| Item | Qty | Spec | Gotchas |
|---|---|---|---|
| MAX98357A I2S amp | 1 | 3 W mono | gain-set pin — leave default 9 dB |
| Speaker | 1 | 40 mm full-range, 4 Ω 3 W | mounts to back panel; flap window leaks plenty of sound, no grille needed |

## 4. Wiring
| Item | Qty | Note |
|---|---|---|
| JST-XH kit 2.54 | 1 box | 2/3/4-pin — one 4-pin (motor) + one 3-pin (hall) per module, MorganManly-style chain |
| Silicone wire 22 AWG | 5 colors | motors usually ship with 1 m leads — verify listing |
| Heatshrink kit | 1 | |
| Perfboard 7×9 cm + screw terminals | 1+10 | the "motherboard" until/unless a PCB happens |
| Dupont jumper stock | — | bring-up only |

## 5. Later (don't order yet)
| Item | Trigger |
|---|---|
| Faceplate — 2 mm aluminium, laser cut | after window trim finalized → export DXF (SendCutSend et al.) |
| Felt/foam liner sheet | with enclosure |
| French cleat | with enclosure |
| Filament top-up: ~1.5 kg black · ~1 kg white · ~0.75 kg red | before production flaps. NOTE: glyph-heavy ring means cols 4-5 carry ~36 **red-bodied** flaps each — red need is way up from the letters-era estimate |

## GPIO budget (C5)
5×STEP + 5×DIR + 1×EN(ganged) + 5×hall + 3×I2S + status LED ≈ **20 pins**. Fits with room; final map at firmware time.

## The three gotchas that actually bite
1. **TMC Vref**: set per-vendor formula to ~1.1-1.2 A RMS. Too low = missed steps under alarm accel; too high = warm drivers holding all day. Standstill auto-reduction (default on) covers the holding-torque requirement — do not disable it.
2. **Hall polarity**: one evening of "sensor is dead" is always a flipped magnet. Test the pair on the bench before assembly.
3. **Same-batch drivers**: five modules from five sellers = five Vref formulas and two silkscreen orientations. One order, one vendor.
