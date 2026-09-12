# Bench wiring — module V1, first power-on

One motor, one driver, one board. Written to be followed in order with nothing
assumed and nothing recalled from memory.

**I cannot give you photographs.** Every connection below is instead a named
source pin, a named destination pin, and how to identify each one on the actual
part in front of you — because the one thing that would make a photograph
useless is a module whose silkscreen differs from mine.

> **THE ILLUSTRATED VERSION IS `docs/wiring/bench-wiring.pdf`** — twenty-one
> pages, one connection per page, drawn parts with their real pin labels, the
> Vref procedure with the meter probes on the pads, a continuity checklist and
> numbered power-on / power-off sequences. Print it and take it to the bench.
>
> Those pages are **generated** by `tools/wiringgen.py` from
> `components/swan_hal/include/hal/pins.h` and **this file**, and the generator
> refuses to emit anything if the two disagree. So: this document is the prose
> source of truth, the pages cannot contradict it, and `tools/wiringgen.py
> --check` (run by `test-host.ps1` and CI) fails the build if they would.
> After editing §2's table, §4's Vref figures or §2's microstep table, re-run
> `python tools/wiringgen.py` and then `tools/wiringrender.ps1`.

**What this round is and is not.** The Hall sensor and magnet are not fitted, so
there is **no homing, no position, and no closed loop**. The firmware cannot
know where the drum is and will not pretend to. What you *can* establish is
everything that does not need a sensor:

- the coil pairing is right
- the microstep setting is really 1/16
- 64 µsteps is really one flap and 3200 is really one revolution
- the direction bit turns the drum the descending way
- 0.7 A is really 0.7 A
- the motor's thermal behaviour over an hour — **which is the whole point of
  gate 3, and it does not need a Hall**

---

## 0. Before anything is powered

Four rules. The first two are how TMC2209s die.

> **1. Never connect or disconnect the motor while VM is on.** An open coil on a
> live driver produces an inductive spike straight into the output stage. This
> kills more StepSticks than every other cause combined. Motor first, power
> second, always.
>
> **2. Never insert or remove the driver module while VM is on.** Same reason,
> plus you will short something on the way past.
>
> **3. VIO before VM, and VM off before VIO.** Logic supply up first so the
> inputs are defined before the output stage has a rail; down last for the same
> reason.
>
> **4. The bulk capacitor goes at the driver's own VM/GND pins.** Not on the
> breadboard rail at the far end. This is the single thing a breadboard build
> gets wrong.

**Breadboard or perfboard?** Breadboard is acceptable at 0.7 A — you are well
inside a contact's rating. Two caveats, and if either bothers you use perfboard:
the four coil wires and the VM/GND pair should be as short and direct as you can
make them, and the bulk cap must bridge the driver's own pins (rule 4), which on
a breadboard means the two holes immediately beside the module, not the rails.
If any contact gets warm, stop and solder it.

---

## 1. The motor — find the coil pairs before you wire anything

Your motor is an **LDO 42STH48-2504AH**: bipolar, 4 wires, 2.5 A/phase rated.
We are running it at **0.7 A RMS**, about 28 % of rated — deliberately, for heat
(spec §5.7a).

The wires are **red, blue, green, black**, and you have cut and re-terminated
them. So do not trust any colour convention, including the one below. Measure.

### 1a. Measure the pairs

You need the DMM on its lowest resistance range (usually 200 Ω).

1. **Touch the two probes together and note the reading.** It will be 0.1–0.5 Ω.
   That is your test leads, and you subtract it from everything that follows.
   Skipping this is how a 1.2 Ω coil reads as 1.6 Ω and you start doubting a
   good motor.

2. **Measure all six combinations** of the four wires and write them down:

   ```
   red   – blue    ______ Ω
   red   – green   ______ Ω
   red   – black   ______ Ω
   blue  – green   ______ Ω
   blue  – black   ______ Ω
   green – black   ______ Ω
   ```

3. **Exactly two of the six will read low** — for a 2.5 A NEMA 17, somewhere
   around **1–2 Ω** after subtracting the leads. Those two pairs are your coils.
   The other **four must read open** (OL / no continuity).

   - If **more than two** read low, you have a short — most likely a strand
     bridging two Dupont pins from the re-termination. Fix it before powering.
   - If **any wire reads low to the motor case**, stop. That is a winding
     shorted to the frame.
   - If a "coil" reads under ~0.3 Ω you are probably measuring a short, not a
     winding.

4. **Cross-check by hand, no meter needed.** Twist one candidate pair's bare
   ends together and turn the shaft. It should become noticeably harder and
   notchier to turn. Untwist, twist the other pair, same effect. Twist a
   *wrong* pair and the shaft turns freely. This takes ten seconds and it has
   caught mistakes that a meter reading did not.

**For reference only, and not to be relied on:** LDO's usual colour convention
on this frame is **black+green = one coil, red+blue = the other**. If your
measurement disagrees with that, **your measurement is right.** The wires have
been cut and re-terminated, which is exactly the circumstance in which a
convention stops being evidence.

### 1b. Wire the coils to the driver

The driver has four motor outputs. On a TMC2209 StepStick they are labelled
**`1A 1B 2A 2B`** — or **`M1A M1B M2A M2B`**, or **`OA1 OA2 OB1 OB2`** — read
your module's silkscreen. `1A/1B` is one H-bridge, `2A/2B` is the other.

> **Connection 1 — coil A**
> ```
> motor wire ____________  ->  driver  1A
> motor wire ____________  ->  driver  1B      (the OTHER end of the SAME coil)
> ```
>
> **Connection 2 — coil B**
> ```
> motor wire ____________  ->  driver  2A
> motor wire ____________  ->  driver  2B      (the OTHER end of the SAME coil)
> ```

**Which coil goes to which bridge does not matter.** Swapping coil A for coil B
reverses rotation, and so does swapping the two ends of one coil — and direction
is a firmware bit on this build (`dir`, §5 below), so there is nothing to get
right here. What matters is only that **each bridge gets both ends of one coil
and never one end of each.** That mistake does not turn the motor; it makes it
buzz, lock, or judder, and it stresses the driver.

Fill the blanks in above once you have measured. They are the record.

---

## 2. Driver ↔ ESP32-C5-DevKitC-1

**Find each driver pin by its silkscreen label**, not by counting positions —
StepStick pinouts differ between vendors and revisions and yours is a FYSETC.
Every pin you need is labelled on the board.

The ESP32 side is fixed and comes from `hal/pins.h`; `pins` on the console
prints it back to you.

| # | driver pin (silkscreen) | goes to | ESP32-C5 pin | notes |
|---|---|---|---|---|
| 3 | `STEP` | → | **GPIO8** | one pulse = one microstep |
| 4 | `DIR` | → | **GPIO24** | level, sampled on the STEP edge |
| 5 | `EN` (or `ENN`, `EN_N`) | → | **GPIO6** | **active LOW** — low enables |
| 6 | `VIO` (or `VDD`) | → | **3V3** | logic rail — **3.3 V, not 5 V** |
| 7 | `GND` (logic side) | → | **GND** | |
| 8 | `GND` (power side) | → | supply − | see §3 |
| 9 | `VM` (or `VMOT`) | → | supply + | see §3 |
| 10 | `MS1` | → | **3V3** | tie high |
| 11 | `MS2` | → | **3V3** | tie high |
| 12 | `PDN` / `PDN_UART` / `UART` | → | see below | standstill current |

### The ones that need explaining

**`VIO` must be 3.3 V.** It sets the logic threshold for STEP, DIR, EN and the
MS pins. Feed it from the DevKitC-1's `3V3` pin. If you feed it 5 V the driver
starts wanting 5 V-ish logic levels from a 3.3 V microcontroller, which works
right up until it doesn't.

**`EN` is active LOW.** Low = drivers on. Most StepStick modules pull EN *high*
on board, so an unconnected or high-Z EN means **disabled**, which is the safe
default and is what you want on a first power-on. Check yours: with VIO powered
and the ESP32 not driving the pin, measure EN — it should read ~3.3 V. **If it
reads near 0 V, add a 10 kΩ resistor from EN to 3V3** before you connect VM,
or the drivers come up energised the instant you apply power.

> Note the deliberate difference from the production design. Spec §2.6 puts a
> 10 kΩ **pulldown** on the EN node so the drivers stay *enabled* through an
> ESP32 reset — because a de-energised drum slews (§5.7). That is right for a
> finished display tracking its position. It is wrong for a first power-on,
> where you want nothing energised until you say so, and this round is not
> tracking position anyway. Use the pull-**up**. Revisit when the interlock is
> built.

**`MS1` and `MS2` both HIGH gives 1/16.** The TMC2209's table is not monotonic,
which is why this is easy to get wrong:

| MS2 | MS1 | microsteps |
|---|---|---|
| 0 | 0 | 1/8 |
| 0 | 1 | 1/2 |
| 1 | 0 | 1/4 |
| **1** | **1** | **1/16** ← what we want |

Tie both to **3V3** (the same 3.3 V as VIO). Modules usually pull them low on
board, so leaving them unconnected gives 1/8 and every constant in the firmware
is then wrong by a factor of two. **§5 has a mechanical check that proves the
setting** — do not take the jumper's word for it.

**`PDN_UART` in standalone mode.** This pin has two jobs. In UART mode it is the
serial line. In standalone mode — which is what we are running — it controls
**automatic standstill current reduction**: after roughly one second without a
step, the driver drops the coil current to about half.

That reduction is not a nicety here. It is the whole thermal contract (§5.7a):
the motor is sealed inside a PLA drum and holds current ~99 % of the day, and
0.7 A run / ~0.35 A hold is what makes that survivable.

**I am not certain of the polarity on your module and will not guess it.**
Vendors add their own pull-ups, pull-downs and jumpers. So:

- Leave `PDN_UART` **as the module ships** for standalone (usually unconnected,
  or on its standalone jumper setting), and
- **verify by measurement in §6.** It is a clean observable: with the motor
  energised and stationary, the supply current should visibly **step down about
  a second after the last step**. If it never steps down, reduction is not
  active, and you tie the pin the other way and re-check.

`VERIFY` — record what you found:

```
PDN_UART left / tied to ____________
standstill step-down observed?  yes / no
supply current, stepping ______ A   holding ______ A
```

### Grounds

Everything shares one ground: ESP32 `GND`, driver logic `GND`, driver power
`GND`, and the supply negative. On a breadboard, run the ESP32's ground to the
same rail the supply negative lands on, and take the driver's two GND pins from
that rail — but keep the **power** ground path short and direct, because that
is the one carrying coil current.

---

## 3. Power

### What to use

You said the 24 V 15 A bench supply is boxed and inconvenient. You do not need
it, and here is a better answer than digging it out:

> **Use the RotoPD trigger, but set it to 9 V for first power-on.** Move to 20 V
> only once the wiring is proven.

Why this is the right call and not a compromise:

- The TMC2209 runs from **4.75 V up**, so 9 V is comfortably inside spec.
- At the speeds this build can reach — capped at 1 drum rev/s — there is no
  voltage headroom needed. One revolution per second is 200 full steps/s, a
  20 ms electrical cycle, against a coil time constant of a couple of
  milliseconds. 9 V has plenty of time to push 0.7 A into the winding.
- A wiring mistake at 9 V dissipates roughly **a fifth** of the energy it would
  at 20 V. That is the entire argument.
- It needs no unpacking and it is the production hardware.

Then **go to 20 V** once the motor has turned correctly, because 20 V is the
production rail and the thermal soak should run on it.

**If you do dig out the bench supply**, it is still the gold standard for one
reason: set the **current limit to ~0.5 A** for the very first power-on and a
wiring error becomes a supply that clicks instead of a driver that dies. If
that is half an hour of your time, it is cheap insurance — but the 9 V PD path
gets you most of the protection for none of it.

**What not to use:** a USB-C charger direct, with no trigger. Without a PD
negotiation it stays at 5 V and, worse, you have no idea what it will do when
the motor's current draw steps.

### The bulk capacitor

> **Connection 13 — bulk capacitance**
> ```
> 100 µF electrolytic  +  ->  driver VM   (the driver's own pin)
> 100 µF electrolytic  -  ->  driver GND  (the driver's own pin)
> ```

- **Voltage rating ≥ 35 V**; your BOM's 50 V part (EEU-FR1H101) is right. Do not
  use a 25 V cap on a 20 V rail — regen from decelerating the drum pushes the
  rail up.
- **Polarity matters.** The stripe is the negative leg. Backwards, an
  electrolytic vents.
- **Legs as short as you can make them.** This cap exists to supply the coil
  current step that the supply lead is too inductive to deliver; twenty
  centimetres of breadboard wire defeats it entirely.

### Power-on order

```
1.  Everything wired, motor INCLUDED, nothing powered
2.  Confirm EN reads high (disabled) — §2
3.  USB-C from the PC to the ESP32          <- VIO comes up, logic defined
4.  Watch the console come up               <- board healthy before any VM
5.  Apply VM (9 V PD)                       <- output stage now live
6.  `en 1` on the console                   <- and only now are coils energised
```

### Power-off order

```
1.  `en 0`            <- de-energise the coils first
2.  Remove VM
3.  Remove USB
```

**If you need to touch the motor wiring at any point: `en 0`, remove VM, and
verify the bulk cap has discharged** — it holds charge after the supply is
gone. Then rewire.

---

## 4. Vref — the 0.7 A setting, measured

### A correction you need before you start

The figures previously in `docs/BRINGUP.md` §28b (≈0.193 V) were **wrong**. They
used `Vref = I × 2.5 × R_sense`, which is the **A4988 / DRV8825** relationship.
The TMC2209 does not work that way: VREF is a scaling input against a full-scale
current fixed by the sense resistor. Setting 0.19 V would have given you about
**0.14 A**, the motor would have skipped under load, and the obvious conclusion
would have been that the drive is too weak. The correct figure is about **1 V**.

### The actual relationship

```
I_RMS(full scale) = V_fs / (R_sense + 0.02 Ω) / √2        V_fs = 0.325 V
I_RMS             = I_RMS(full scale) × (Vref / 2.5 V)
```

Which gives, for the two sense resistors these modules ship with:

| R_sense | full-scale I_RMS | **Vref for 0.7 A RMS** |
|---|---|---|
| **0.11 Ω** | 1.77 A | **0.99 V** |
| **0.15 Ω** | 1.35 A | **1.29 V** |

### Step 1 — read the sense resistor, do not guess it

Two small SMD resistors sit near the motor output pins. Read the marking:

- **`R110`** → 0.11 Ω → target Vref **0.99 V**
- **`R150`** → 0.15 Ω → target Vref **1.29 V**
- **`R100`** → 0.10 Ω → target Vref 0.91 V

```
sense resistor marking : ______________   ->  target Vref ______ V
```

If they are unreadable, use 0.11 Ω (much the more common) and treat §6's
current check as the thing that confirms it.

### Step 2 — set it

**Motor disconnected. VM off. USB on.** The VREF divider is fed from the logic
rail, so VIO must be up — but the output stage must not be.

1. DMM to **DC volts, 2 V range**. Black probe on driver `GND`.
2. Red probe on the **VREF test point** if your module has one (a labelled pad —
   FYSETC usually provides one). **If it does not**, the trimpot's metal screw
   *is* the wiper. Use a clip lead, not a hand-held probe: a slipped probe here
   shorts the wiper to a neighbouring pad and kills the driver.
3. Turn the pot in small increments and read directly. Clockwise is usually up;
   confirm on your board rather than assuming.
4. Set to the target from step 1.

```
Vref set               : ______ V
driver silkscreen rev  : ______________
date / who             : ______________
```

### Step 3 — confirm it is really 0.7 A

You have a scope, which makes this a real measurement rather than a guess.

**Peak coil current at 0.7 A RMS is 0.99 A** (0.7 × √2).

- **With the scope and a low-side shunt** (a 0.1 Ω, ≥1 W resistor in series with
  one coil, scope across it): energise, step slowly, and you will see the
  chopped current regulating. Peak coil current should reach ~0.99 A at the
  microstep positions where that phase is at full amplitude. This also shows you
  whether the chopper is behaving.
- **With the DMM alone**: put it in series with **one** coil on the 10 A range —
  **with the power off while you insert it** — energise, and step slowly with
  `step 0 1` repeatedly. The reading sweeps between roughly 0 and 0.99 A as the
  microstep angle rotates through that phase. **The maximum you see over a full
  electrical cycle is the peak**, and it should be ~0.99 A.
- **Your clamp meter is probably not useful here.** Most are AC-only and a
  stepper coil at standstill is DC. Only a Hall-effect DC clamp will read it,
  and even then 1 A is near the bottom of its range.

```
measured peak coil current : ______ A   (target 0.99 A)
method                     : shunt+scope / DMM in series / not measured
```

If you cannot measure it honestly, **write "not measured"** rather than a number
you inferred. An unverified current invalidates the thermal result, which is the
only thing gate 3 exists to produce.

### On torque margin, so it is not a surprise

Spec §5.7a claims ~2× margin at 0.7 A against a reflected demand of ~9.1 N·cm.
That comes from the mechanical handoff's torque figure. Scaling your motor's
*rated* holding torque linearly by current gives a lower number — somewhere
around **1.4×**. Both are estimates and they disagree.

The bench settles it: **if the drum skips steps at 0.7 A, that is the torque
margin talking.** Do not wind the current up to fix it — you set a 0.7 A hard
limit for this round and the thermal answer depends on it. Stop, record it, and
we decide with a number in hand.

---

## 5. Flash and run

### Build and flash the bench image

```bash
.\build.ps1 -B build-bench -DSWAN_BENCH=ON set-target esp32c5
```

```bash
.\build.ps1 -B build-bench -DSWAN_BENCH=ON build
```

```bash
.\build.ps1 -B build-bench -DSWAN_BENCH=ON -p COM3 app-flash monitor
```

`app-flash` only — it swaps the app and leaves NVS and the filesystem alone, so
your settings survive and reverting is one more `app-flash` of the normal build.

It announces itself as **`0.4.0+devkitc1.bench`** in the boot log. If it does
not say `bench`, you are running the wrong image and the speed cap is not there.

**The cap:** every commanded rate is refused above **50 flaps/s = 1 drum rev/s**.
`spin 0 400 5` prints a refusal rather than running. This was a real hole until
today — the cap was only applied to *configured* speeds and an explicit `spin`
walked past it. It is enforced inside `motion::step_open_loop` now, which is the
one function every commanded rate reaches.

### First: stop it trying to home

With no Hall fitted, the board will home all five columns at boot, fail, retry
three times each and latch faults. That is correct behaviour and it costs you
~30 s of confusing console output every boot.

```
maint on
```

Maintenance survives a reboot, does not home, and leaves EN released — which is
exactly the state you want for a hall-less bench. Manual commands still work;
that is what maintenance is for.

```
col 0 real
en 1
```

### Prove the geometry — the check that replaces homing

This is the most valuable thing you will do this session, and it needs no
sensor. **Mark the drum** with a pen at some reference against the frame.

```
step 0 3200
```

**That must be exactly one revolution** — the mark returns to where it started.

`step` runs at `flaps_s_home`, 8 flaps/s, so this takes about **6.3 s**.
Slow enough to watch, which is the point.

It is a complete test of the whole chain at once:

| what you see | what it means |
|---|---|
| **exactly 1 revolution** | 1/16 microstepping, 3200 µsteps/rev, 1:1 drive — all correct |
| 2 revolutions | MS pins are giving **1/8** — MS1/MS2 not actually high |
| 4 revolutions | 1/4 |
| 8 revolutions | 1/2 |
| a fraction of a revolution | not 1:1 — something is geared, or the drum slipped on the coupling |
| judder, no net rotation | **coil pairing is wrong** — go back to §1 |

```
step 0 3200 gave : ______ revolutions
```

Then the flap:

```
step 0 64
```

One flap: **7.2° of drum**, one fiftieth of a turn. Do it fifty times and you
should be back at the mark.

### The direction bit

The ring is **descending**: one forward flip must **decrement** the displayed
digit (spec §4). With the motor now inside the drum facing the other way, which
DIR level does that is not knowable on paper.

```
dir
```

shows the current setting.

```
dir 1
```

flips it. Watch a `step 0 64` and check which way the cards go. When it is
right:

```
save
```

```
dir_invert settled at : 0 / 1
```

### The slow spin

```
spin 0 25 20
```

Twenty seconds at 25 flaps/s — half the cap. Then, if that looked right:

```
spin 0 50 20
```

One drum revolution per second, the fastest this image will go. Watch for
runout, wobble, anything rubbing, and how the motor leads behave.

Try `spin 0 400 5` **once**, so you have seen the refusal work rather than
trusting that it does.

### The one-flap-per-tick soak

```
bench soak 0
```

Sixty minutes, one flap per second. Leave it alone; `bench` shows progress,
`bench stop` aborts.

**With no Hall fitted this runs OPEN LOOP** and says so in the log and in the
final report. That is deliberate and it was changed today: the soak used to call
the closed-loop `go()`, which refuses without a home reference, so on your
module it would have sat there for an hour doing nothing. The thermal question
does not need a Hall — **the heat is in the holding current** — so it now falls
back to open-loop flaps and omits the edge figures rather than printing zeroes
that look like clean results.

What you get: the thermal answer, which is what gate 3 is for.
What you do not get: `hall_to_hall`, resyncs, edge error. Those wait for the
magnet.

At the end it stops and asks you to put a hand on the motor case.

---

## 6. What to watch, and what "stop now" looks like

### Watch continuously

- **The motor case temperature.** Hand on it every few minutes early on. It
  should settle warm, not climb.
- **The driver chip.** It has no heatsink obligation at 0.7 A, but it should not
  be too hot to touch either.
- **The supply current.** With a scope or the supply's own readout: it should be
  steady, and it should **step down about a second after motion stops** — that
  is standstill reduction working, and it is the §2 `PDN_UART` verification.
- **Sound.** A steady hiss at standstill is the chopper and is normal. A
  changing pitch, a rattle or a grinding note is not.

### STOP NOW — cut VM immediately

| what you observe | what it is |
|---|---|
| **any smell of hot plastic or varnish** | stop first, diagnose second |
| **the driver too hot to keep a finger on** | over-current, or a coil short |
| **the motor case climbing past "uncomfortable to hold"** early in the run | current is wrong — recheck Vref against §4, not the motor |
| **the bulk cap warm, or bulging** | wrong voltage rating, or reversed |
| **loud buzzing with no rotation** | coil pairing wrong (§1) — this also stresses the driver |
| **supply current spiking or the supply cutting out** | short, or a coil intermittently open at a Dupont pin |
| **anything rubbing or the drum wobbling visibly** | mechanical, and the flaps are at risk |
| **the ESP32 rebooting repeatedly** | brownout — the logic rail is sagging; check grounds |

De-energise with `en 0` if the console is responsive, but **do not go looking
for the console if something smells hot.** Pull VM.

### Expected, and not a problem

- **Faults on columns 1–4.** Nothing is wired to them; they will latch
  `no_hall`. Ignore them.
- **A `no_hall` fault on column 0 too**, before you enter maintenance. Also
  expected — there is no magnet.
- **The drum settling to a slightly different rest position** after `en 0`. The
  drum is unbalanced 3.92 N·cm against a 2.2 N·cm detent (§5.7), so an
  unpowered drum slews to its heavy side. That is the physics this whole
  holding-current contract exists for, and seeing it confirms the premise.
- **A steady hiss at standstill.** The chopper.

### Record before you pack up

Everything in the blanks above, plus BRINGUP §28b gate 3's own blocks. An
unrecorded number is a session that has to happen twice — and this one takes an
hour of it just waiting.
