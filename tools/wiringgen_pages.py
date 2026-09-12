#!/usr/bin/env python3
"""The page content for docs/wiring/.  Imported by tools/wiringgen.py.

Every pin number that appears on a page arrives here as an argument, read from
hal/pins.h and docs/BENCH_WIRING.md by the caller.  Nothing in this file
hard-codes a GPIO.
"""

from wiringgen import (
    Page, W, H, INK, MUTE, FAINT, DANGER, GOOD, WARN, COLOURS, MOTOR_WIRES,
    TMC_X, TMC_Y, TMC_W, TMC_H, ESP_X, ESP_Y, ESP_W, ESP_H, MOT_X, MOT_Y,
    PSU_X, PSU_Y, PSU_W, PSU_H,
    draw_tmc, draw_esp, draw_motor, draw_psu, run,
    tmc_left_xy, tmc_right_xy, esp_xy, tmc2209_vref, FACTS,
)


# --------------------------------------------------------------------------
def conn_page(num, cnum, drv, dest, key, what, why, warn=None, warn_kind="danger",
              done=(), volts="9 V", extra=None):
    """One connection, drawn big.  `done` lists driver pins already wired."""
    nm, col, sw = COLOURS[key]
    p = Page(num, "Connection %s  —  %s" % (cnum, what),
             "wire colour: %s" % nm)
    hot = [drv] if isinstance(drv, str) else list(drv)
    # The module has two pins labelled GND.  Highlighting both would defeat the
    # point of a page-per-connection: index 0 is the power ground beside VM,
    # index 1 the logic ground beside VIO.
    gnd = 0 if dest == "psu" else 1
    draw_tmc(p, hot=tuple((n, gnd) if n == "GND" else n for n in hot))

    if dest == "esp3v3":
        draw_esp(p, hot=("3V3",))
        a, b = tmc_left_xy(hot[0]), esp_xy("3V3")
        run(p, [a, (520, a[1]), (520, b[1]), b], key)
    elif dest.startswith("esp:"):
        pin = dest.split(":", 1)[1]
        draw_esp(p, hot=(pin,))
        a, b = tmc_left_xy(hot[0]), esp_xy(pin)
        run(p, [a, (520, a[1]), (520, b[1]), b], key)
    elif dest == "esp-around":
        draw_esp(p, hot=("3V3" if hot[0] == "VIO" else "GND",))
        a = tmc_right_xy(hot[0], 1 if hot[0] == "GND" else 0)
        b = esp_xy("3V3" if hot[0] == "VIO" else "GND")
        run(p, [a, (960, a[1]), (960, 700), (470, 700), (470, b[1]), b], key)
        p.text(715, 692, "route under the module", 15, col, "middle", "bold")
    elif dest == "none":
        p.rect(1010, 300, 500, 200, fill="#fdf7ec", stroke=WARN, sw=3, rx=8, dash="12 8")
        p.text(1260, 372, "LEAVE AS THE MODULE SHIPS", 21, WARN, "middle", "bold")
        p.text(1260, 410, "for standalone operation", 17, MUTE, "middle")
        p.text(1260, 452, "then VERIFY on page 21", 17, INK, "middle", "bold")
        a = tmc_left_xy(hot[0])
        run(p, [a, (a[0] - 90, a[1])], key, dash="10 8")
        p.text(a[0] - 100, a[1] + 6, "?", 26, col, "end", "bold")
    elif dest == "psu":
        draw_psu(p, volts)
        pos, neg = (PSU_X - 22, PSU_Y + 44), (PSU_X - 22, PSU_Y + 106)
        a = tmc_right_xy("VM") if hot[0] == "VM" else tmc_right_xy("GND", 0)
        b = pos if hot[0] == "VM" else neg
        run(p, [a, (1080, a[1]), (1080, b[1]), b], key)
    p.rect(52, 730, 1496, 2, fill=FAINT, stroke="none")
    y = p.wrap(52, 776, why, 19, INK, cols=104, lh=28)
    if warn:
        p.banner(52, y + 16, 1000, warn, warn_kind, size=17)
    if extra:
        extra(p)
    return p


# ============================== 1 ==========================================
def page01(pins, table):
    p = Page(1, "Bench wiring — module V1", "First power-on, connection by connection")
    y = p.wrap(52, 148,
               "The whole job in twenty-one pages, in the order you should do it. One "
               "connection per page. Every pin number here is read out of hal/pins.h and "
               "docs/BENCH_WIRING.md when these pages are rendered, so a drawing cannot "
               "quietly disagree with the firmware.", 19, INK, cols=64, lh=28)
    y = p.banner(52, y + 14, 720,
                 "The Hall sensor and magnet are NOT fitted. No homing, no position, no "
                 "closed loop this round. That is expected, not a fault.", "warn")
    y = p.banner(52, y + 8, 720,
                 "READ PAGE 2 FIRST. The three rules on it are how TMC2209 drivers die.",
                 "danger")
    p.text(52, y + 52, "THE ORDER, and why it is this order", 20, INK, weight="bold")
    # Two columns, because SVG collapses runs of spaces and a padded mono
    # string comes out ragged.
    steps = [("1-4", "read the rules, identify the parts, find the coil pairs", 0),
             ("5-12", "wire the logic  (driver to ESP32)", 0),
             ("13-15", "wire the power  (supply, and the bulk capacitor)", 0),
             ("16", "continuity check  <-  find mistakes with a meter", 1),
             ("17", "set Vref  <-  motor still NOT connected", 1),
             ("18-19", "connect the motor  <-  power off while you do it", 1),
             ("20-21", "finished layout, power on / power off", 0)]
    for i, (num, txt, strong) in enumerate(steps):
        yy = y + 88 + i * 30
        p.mono(60, yy, num, 17, INK if strong else MUTE,
               weight="bold" if strong else "normal")
        p.mono(148, yy, txt, 17, INK if strong else MUTE,
               weight="bold" if strong else "normal")

    kx, ky = 850, 148
    p.text(kx, ky, "WIRE COLOURS — the same on every page", 20, INK, weight="bold")
    p.rect(kx - 14, ky + 20, 700, 452, fill="#fafbfc", stroke=FAINT, sw=2, rx=6)
    label = {"GNDL": "GND  (logic)", "GNDP": "GND  (power)", "VIO": "VIO  ->  3V3",
             "VM": "VM  (motor +)"}
    for i, k in enumerate(["VM", "GNDP", "VIO", "GNDL", "STEP", "DIR", "EN",
                           "MS1", "MS2", "PDN"]):
        nm, col, sw = COLOURS[k]
        yy = ky + 58 + i * 41
        p.line(kx + 8, yy, kx + 100, yy, col, sw)
        p.mono(kx + 120, yy + 6, label.get(k, k), 17, INK)
        p.text(kx + 350, yy + 6, nm, 17, MUTE)
    p.text(kx - 6, ky + 500, "The motor keeps its own colours: red, blue, green, black.",
           16, MUTE)
    p.text(kx - 6, ky + 528, "Use any wire you like — but be consistent, and", 16, MUTE)
    p.text(kx - 6, ky + 552, "never use red or black for a signal.", 16, INK, weight="bold")
    return p


# ============================== 2 ==========================================
def page02(pins, table):
    p = Page(2, "Three ways to kill the driver", "Read before you pick anything up")
    y = 132
    items = [
        ("1", "NEVER connect or disconnect the motor while VM is live.",
         "An open coil on a live driver dumps its stored energy into the output stage. "
         "This kills more StepSticks than every other cause put together. Power down "
         "FIRST, then touch the motor plug — every time, including 'just for a second'."),
        ("2", "NEVER insert or remove the driver module while VM is live.",
         "Same physics, and you will bridge something on the way past as well."),
        ("3", "Set Vref BEFORE the motor is ever connected.",
         "Vref sets the coil current. Set it with the motor attached and the first thing "
         "your motor sees is wherever the pot happened to be when it left the factory — "
         "often well over 1.5 A. This guide sets Vref on page 17 and connects the motor "
         "on pages 18-19, in that order, deliberately."),
    ]
    for n, head, body in items:
        p.rect(52, y, 1496, 176, fill="#fdf0ef", stroke=DANGER, sw=3, rx=8)
        p.rect(52, y, 10, 176, fill=DANGER, stroke="none")
        p.circle(116, y + 60, 32, DANGER)
        p.text(116, y + 71, n, 32, "#ffffff", "middle", "bold")
        p.text(174, y + 50, head, 24, DANGER, weight="bold")
        p.wrap(174, y + 88, body, 17, INK, cols=112, lh=26)
        y += 196
    p.text(52, y + 40, "And one that is less dramatic but more common", 20, INK, weight="bold")
    p.wrap(52, y + 74,
           "The bulk capacitor must sit at the driver's OWN VM and GND pins — not on a "
           "breadboard rail at the far end of the board. Page 15 shows where.",
           18, INK, cols=98, lh=26)
    p.wrap(52, y + 146,
           "If you are unsure at any point: power off, and nothing you do next can break "
           "anything. Every irreversible mistake in this guide needs VM to be live.",
           17, GOOD, cols=104, lh=25)
    return p


# ============================== 3 ==========================================
def page03(pins, table):
    p = Page(3, "Identify your two parts", "Match by LABEL — drawn positions are only a hint")
    draw_tmc(p)
    draw_esp(p)
    p.banner(1010, 250, 520,
             "Pin ORDER here is the standard StepStick arrangement. If your module's "
             "silkscreen puts a label somewhere else, THE LABEL WINS. Every instruction "
             "in this guide names a pin and never a position.", "warn", size=16)
    p.text(1010, 470, "Tick off these labels on the driver:", 18, INK, weight="bold")
    labs = ["EN", "MS1", "MS2", "PDN / UART", "STEP", "DIR",
            "VM / VMOT", "GND  (x2)", "1A", "1B", "2A", "2B", "VIO / VDD"]
    for i, lab in enumerate(labs):
        cx = 1010 + (i % 2) * 270
        cy = 508 + (i // 2) * 36
        p.rect(cx, cy - 15, 20, 20, fill="none", stroke=INK, sw=2, rx=3)
        p.mono(cx + 32, cy, lab, 16, INK)
    p.text(52, 760, "Find the two SENSE RESISTORS as well", 20, INK, weight="bold")
    y = p.wrap(52, 794,
               "Two small surface-mount parts beside the motor output pins, marked R110, "
               "R150 or R100. That marking decides your Vref target on page 17, and reading "
               "it takes ten seconds where guessing it costs you the whole thermal result.",
               18, INK, cols=94, lh=26)
    p.rect(52, y + 12, 420, 44, fill="none", stroke=INK, sw=2, rx=4)
    p.mono(68, y + 42, "R _____   =   0. _____ ohm", 19, MUTE)
    p.text(560, y + 42, "R110 = 0.11 ohm  ·  R150 = 0.15 ohm  ·  R100 = 0.10 ohm",
           17, MUTE)
    return p


# ============================== 4 ==========================================
def page04(pins, table):
    p = Page(4, "Find the coil pairs", "Motor connected to NOTHING. Meter only.")
    draw_motor(p, x=200, y=180)
    mx, my = 560, 200
    p.rect(mx, my, 230, 300, fill="#f6f8fa", stroke=INK, sw=3, rx=10)
    p.rect(mx + 22, my + 28, 186, 74, fill="#dfe6ea", stroke=INK, sw=2, rx=4)
    p.mono(mx + 115, my + 78, "1.4", 36, INK, "middle", "bold")
    p.text(mx + 115, my + 130, "range: 200 ohm", 15, MUTE, "middle")
    p.circle(mx + 115, my + 192, 42, "#e9edf1", INK, 2)
    p.line(mx + 115, my + 192, mx + 115, my + 158, INK, 4)
    p.line(mx + 55, my + 272, mx + 55, my + 300, "#22262b", 7)
    p.line(mx + 175, my + 272, mx + 175, my + 300, "#d0342c", 7)
    p.text(mx + 115, my - 18, "DMM", 19, INK, "middle", "bold")

    y = p.wrap(850, 190,
               "The wires were cut and re-terminated, so no colour convention is evidence "
               "any more. Measure all six combinations. Exactly two read low — those are "
               "the coils. The other four must read open.", 18, INK, cols=58, lh=26)
    y = p.wrap(850, y + 8,
               "FIRST touch the probes together and note the reading (0.1-0.5 ohm). That is "
               "your test leads. Subtract it from everything below.", 17, WARN, cols=60, lh=24)
    yy = y + 16
    for c in ["red  -  blue", "red  -  green", "red  -  black",
              "blue  -  green", "blue  -  black", "green  -  black"]:
        p.mono(866, yy, c, 18, INK)
        p.line(1120, yy + 6, 1310, yy + 6, FAINT, 2)
        p.text(1322, yy, "ohm", 15, MUTE)
        yy += 38
    p.banner(850, yy + 6, 690,
             "Two low (about 1-2 ohm), four open. More than two low is a short — usually "
             "a stray strand on a Dupont pin. Any wire beeping to the motor CASE: stop, "
             "that winding is shorted to the frame.", "danger", size=16)

    y2 = p.wrap(52, 620,
                "CROSS-CHECK WITH NO METER: twist one candidate pair's bare ends together "
                "and turn the shaft. A real pair makes it noticeably harder and notchier to "
                "turn; a wrong pair turns freely. Ten seconds, and it catches mistakes a "
                "meter reading does not.", 17, INK, cols=72, lh=25)
    p.text(52, y2 + 34, "Write it down — pages 18 and 19 refer to it:", 19, INK, weight="bold")
    for i, lab in enumerate(("PAIR 1  =", "PAIR 2  =")):
        p.mono(52, y2 + 84 + i * 50, lab, 20, INK, weight="bold")
        p.line(196, y2 + 90 + i * 50, 620, y2 + 90 + i * 50, INK, 2)
    p.wrap(52, y2 + 200,
           "Expected for this motor: black+green one coil, red+blue the other. IF YOUR "
           "METER DISAGREES, THE METER WINS.", 17, WARN, cols=72, lh=24)
    return p


# ========================= 5 .. 14  the connections ========================
def make_conn_pages(pins, table):
    g = "GPIO%d"
    done = []
    out = []

    def add(page_no, cnum, drv, dest, key, what, why, warn=None, wk="danger",
            extra=None):
        out.append(conn_page(page_no, cnum, drv, dest, key, what, why, warn, wk,
                             done=tuple(done), extra=extra))
        done.append(drv if isinstance(drv, str) else drv[0])

    add(5, "3", "STEP", "esp:" + g % pins["STEP"], "STEP",
        "STEP  ->  " + g % pins["STEP"],
        "One pulse on this wire moves the motor one microstep. At 1/16 microstepping "
        "that is 1/64th of a flap, and 3200 pulses is exactly one drum revolution. This "
        "is the only wire that carries motion.")

    add(6, "4", "DIR", "esp:" + g % pins["DIR"], "DIR",
        "DIR  ->  " + g % pins["DIR"],
        "A level, not a pulse: the driver reads it on each STEP edge to decide which way "
        "to turn. The ring is descending, so getting this backwards makes a countdown "
        "count up — which is why it is a firmware bit you can flip with `dir` rather than "
        "something to get right with a soldering iron.")

    add(7, "5", "EN", "esp:" + g % pins["EN"], "EN",
        "EN  ->  " + g % pins["EN"],
        "ENABLE, and it is ACTIVE LOW: pulling it low turns the coils on. Most modules "
        "pull it high on board, so an unconnected EN means disabled — which is the safe "
        "default you want for a first power-on.",
        "CHECK YOURS. With USB in and VM off, measure EN against GND: it should read "
        "about 3.3 V. If it reads near 0 V, add a 10 kohm resistor from EN to 3V3 before "
        "you ever connect VM, or the coils energise the instant power arrives.", "warn")

    add(8, "6", "VIO", "esp-around", "VIO",
        "VIO  ->  3V3",
        "The logic supply. It sets the voltage the driver expects to see on STEP, DIR, EN "
        "and the MS pins, so it must be the SAME 3.3 V the ESP32 drives with. VIO is on "
        "the right-hand pin column, so this one routes under the module.",
        "3.3 V, not 5 V. Feeding VIO 5 V makes the driver want 5 V logic levels from a "
        "3.3 V microcontroller. It appears to work, right up until it does not.", "danger")

    add(9, "7", "GND", "esp-around", "GNDL",
        "GND (logic)  ->  GND",
        "The logic ground. Without a shared ground the ESP32 and the driver have no common "
        "reference and every signal level is meaningless. This is the wire whose absence "
        "produces the most baffling symptoms.")

    add(10, "10", "MS1", "esp3v3", "MS1",
        "MS1  ->  3V3",
        "Microstep select, bit one. Tie it HIGH.")

    def ms_table(p):
        # Straight out of BENCH_WIRING.md, checked by wiringgen.verify(); the
        # order is genuinely not monotonic and retyping it is how it gets wrong.
        tx, ty = 960, 250
        p.text(tx, ty, "TMC2209 microstep table", 19, INK, weight="bold")
        p.rect(tx - 14, ty + 18, 540, 42 + 38 * len(FACTS["ms"]),
               fill="#fafbfc", stroke=FAINT, sw=2, rx=6)
        for j, h in enumerate(("MS2", "MS1", "microsteps")):
            p.text(tx + 10 + j * 110, ty + 48, h, 16, MUTE, weight="bold")
        for i, (a, b, r) in enumerate(FACTS["ms"]):
            want = (a, b) == (1, 1)
            yy = ty + 86 + i * 38
            for j, cell in enumerate((str(a), str(b), r)):
                p.mono(tx + 10 + j * 110, yy, cell, 19, INK if want else MUTE,
                       weight="bold" if want else "normal")
            if want:
                p.text(tx + 350, yy, "<- what we want", 17, GOOD, weight="bold")

    add(11, "11", "MS2", "esp3v3", "MS2",
        "MS2  ->  3V3",
        "Microstep select, bit two. Tie it HIGH as well. BOTH high is 1/16 on a TMC2209, "
        "and the table is not in a sensible order — which is exactly why this one is easy "
        "to get wrong.",
        "Modules usually pull MS1 and MS2 LOW on board. Leave them unconnected and you get "
        "1/8, which makes every constant in the firmware wrong by a factor of two. Page 20 "
        "has a mechanical check that proves the setting — do not trust the jumper.", "warn",
        extra=ms_table)

    add(12, "12", "PDN", "none", "PDN",
        "PDN / UART  —  standstill current",
        "In standalone mode this pin controls automatic standstill current reduction: about "
        "a second after the last step, the driver drops the coil current to roughly half. "
        "That is not a nicety here — it is the entire thermal contract, because the motor "
        "is sealed in a PLA drum and holds current all day.",
        "THE POLARITY IS NOT VERIFIED for your module and this guide will not guess it. "
        "Leave PDN as the module ships for standalone, then confirm on page 21: with the "
        "motor energised and still, the supply current must visibly STEP DOWN about a "
        "second after the last step. If it never does, tie the pin the other way and "
        "re-check.", "warn")

    add(13, "9", "VM", "psu", "VM",
        "VM  ->  supply  +",
        "The motor supply. Start at 9 V from the PD trigger, not 20 V: the TMC2209 runs "
        "from 4.75 V up, at one drum revolution per second there is no headroom needed, "
        "and a wiring mistake at 9 V dissipates about a fifth of the energy. Move to 20 V "
        "once the wiring is proven.")

    add(14, "8", "GND", "psu", "GNDP",
        "GND (power)  ->  supply  -",
        "The power ground — the wire that carries the coil current back. Keep it short and "
        "direct. This is a different job from the logic ground on page 9 even though they "
        "are the same net; on a breadboard, route it separately and let them meet at the "
        "supply.")
    return out


# ============================== 15 =========================================
def page15(pins, table):
    p = Page(15, "Bulk capacitor  —  connection 13", "100 uF, at the driver's OWN pins")
    draw_tmc(p, hot=("VM", ("GND", 0)))
    a, b = tmc_right_xy("VM"), tmc_right_xy("GND", 0)
    cx, cy, cw, ch = 985, 232, 118, 188
    p.rect(cx, cy, cw, ch, fill="#f0f2f5", stroke=INK, sw=3, rx=10)
    p.rect(cx + cw - 30, cy + 3, 27, ch - 6, fill="#c7ced6", stroke="none", rx=6)
    for k in range(4):
        p.text(cx + cw - 16, cy + 52 + k * 34, "-", 24, INK, "middle", "bold")
    p.text(cx + 44, cy + 88, "100", 27, INK, "middle", "bold")
    p.text(cx + 44, cy + 122, "uF", 22, INK, "middle")
    p.text(cx + 44, cy + 152, "35 V+", 15, MUTE, "middle")
    # legs: + at the top, - at the bottom, clear of the body
    p.line(cx + 40, cy, cx + 40, cy - 46, INK, 4)
    p.line(cx + 88, cy + ch, cx + 88, cy + ch + 46, INK, 4)
    p.text(cx + 40, cy - 56, "+", 26, "#d0342c", "middle", "bold")
    p.text(cx + 116, cy + ch + 30, "-", 30, "#22262b", "middle", "bold")
    run(p, [a, (940, a[1]), (940, cy - 46), (cx + 40, cy - 46)], "VM")
    run(p, [b, (962, b[1]), (962, cy + ch + 46), (cx + 88, cy + ch + 46)], "GNDP")
    p.banner(1170, 240, 378,
             "THE STRIPE IS THE NEGATIVE LEG. Backwards, an electrolytic vents.",
             "danger", size=16)
    p.banner(1170, 390, 378,
             "35 V minimum; your 50 V part is right. Never a 25 V cap on the 20 V rail "
             "— decelerating the drum pushes the rail up.", "warn", size=16)
    p.rect(52, 730, 1496, 2, fill=FAINT, stroke="none")
    y = p.wrap(52, 776,
               "This capacitor supplies the sudden current the coils demand at each step, "
               "which the supply lead is too inductive to deliver. Legs as short as you can "
               "make them, bridging the driver's own VM and GND pins.", 19, INK, cols=104, lh=28)
    p.banner(52, y + 16, 1000,
             "ON A BREADBOARD THIS MEANS THE TWO HOLES BESIDE THE MODULE, not the power "
             "rails at the end of the board. Twenty centimetres of breadboard wire defeats "
             "the capacitor entirely. This is the single thing breadboard builds get wrong.",
             "danger", size=17)
    return p


# ============================== 16 =========================================
def page16(pins, table):
    p = Page(16, "Continuity check", "Beep these five pairs BEFORE the first power-on")
    y = p.wrap(52, 140,
               "Meter on continuity (the beep symbol). Everything unpowered, USB out, motor "
               "still disconnected. Five checks: three that must NOT beep, two that MUST. "
               "A mistake found here costs a minute; the same mistake found by powering up "
               "costs a driver.", 19, INK, cols=98, lh=28)
    rows = [("VM", "GND", False, "A short across the supply. This is the one that makes smoke."),
            ("VM", "3V3", False, "Motor voltage on the ESP32's 3.3 V rail — destroys the board."),
            ("3V3", "GND", False, "Shorts the ESP32's regulator; the board will not boot."),
            ("ESP GND", "driver GND", True, "No common ground means every logic level is "
                                            "undefined. Nothing works, confusingly."),
            ("ESP 3V3", "driver VIO", True, "MS1 and MS2 must beep to 3V3 as well — they are "
                                            "on the same net.")]
    yy = y + 24
    for i, (a, b, must, why) in enumerate(rows):
        col = GOOD if must else DANGER
        bg = "#eff8f2" if must else "#fdf0ef"
        p.rect(52, yy, 1496, 118, fill=bg, stroke=col, sw=3, rx=8)
        p.rect(52, yy, 9, 118, fill=col, stroke="none")
        p.circle(112, yy + 44, 26, col)
        p.text(112, yy + 54, str(i + 1), 26, "#ffffff", "middle", "bold")
        p.mono(166, yy + 40, "%-12s  <-->  %s" % (a, b), 22, INK, weight="bold")
        p.text(166, yy + 76, "MUST BEEP" if must else "MUST NOT BEEP",
               20, col, weight="bold")
        p.wrap(620, yy + 40, why, 17, INK, cols=62, lh=24)
        p.rect(1476, yy + 30, 30, 30, fill="none", stroke=INK, sw=2, rx=4)
        yy += 130
    p.wrap(52, yy + 26,
           "All five correct? Then the first power-on cannot destroy anything through a "
           "wiring error. Go to page 17.", 18, GOOD, cols=116, lh=26)
    return p


# ============================== 17 =========================================
def page17(pins, table):
    v011, f011 = tmc2209_vref(0.7, 0.11)
    v015, f015 = tmc2209_vref(0.7, 0.15)
    # The subtitle is the guide's own precondition sentence, verbatim.  It used
    # to read "VM ON", which BENCH_WIRING.md section 4 step 2 flatly contradicts.
    p = Page(17, "Set Vref  —  0.7 A", FACTS["vref_pre"])
    # zoomed module corner with the pot and probes
    bx, by = 90, 150
    p.rect(bx, by, 520, 388, fill="#f6f8fa", stroke=INK, sw=3, rx=8)
    p.text(bx + 260, by - 16, "driver, close up", 18, MUTE, "middle")
    p.circle(bx + 150, by + 150, 58, "#dfe4ea", INK, 3)
    p.line(bx + 112, by + 150, bx + 188, by + 150, INK, 7)
    p.text(bx + 150, by + 236, "Vref trimpot", 17, INK, "middle", "bold")
    p.text(bx + 150, by + 258, "(the screw head IS the wiper)", 14, MUTE, "middle")
    # probes: both leads leave to the right, clear of the captions under the pot
    p.line(bx + 150, by + 150, bx + 330, by + 60, "#d0342c", 9)
    p.circle(bx + 150, by + 150, 9, "#d0342c")
    p.text(bx + 340, by + 54, "RED probe", 17, "#d0342c", weight="bold")
    p.text(bx + 340, by + 76, "on the wiper", 14, MUTE)
    p.rect(bx + 24, by + 300, 120, 28, fill="#9aa4ae", stroke=INK, sw=2, rx=3)
    p.mono(bx + 84, by + 320, "GND", 15, INK, "middle", "bold")
    # dog-legged BELOW the trimpot caption rather than straight across it
    p.poly([(bx + 144, by + 314), (bx + 210, by + 314), (bx + 330, by + 196)],
           stroke="#22262b", sw=9)
    p.circle(bx + 144, by + 314, 9, "#22262b")
    p.text(bx + 340, by + 190, "BLACK probe", 17, "#22262b", weight="bold")
    p.text(bx + 340, by + 212, "on any GND pin", 14, MUTE)
    p.banner(bx, by + 400, 520,
             "If your module has a labelled VREF pad, use that instead of the screw. "
             "Use a CLIP, not a hand-held probe: one slip from the wiper to a "
             "neighbouring pad kills the driver.", "danger", size=16)

    p.text(700, 176, "1.  Read your sense resistor (page 3)", 20, INK, weight="bold")
    p.rect(700, 196, 840, 158, fill="#fafbfc", stroke=FAINT, sw=2, rx=6)
    hdr = ["marking", "R_sense", "full scale", "Vref for 0.7 A"]
    for j, hcell in enumerate(hdr):
        p.text(724 + j * 210, 228, hcell, 16, MUTE, weight="bold")
    data = [("R110", "0.11 ohm", "%.2f A" % f011, "%.2f V" % v011),
            ("R150", "0.15 ohm", "%.2f A" % f015, "%.2f V" % v015)]
    for i, rowd in enumerate(data):
        for j, cell in enumerate(rowd):
            p.mono(724 + j * 210, 268 + i * 38, cell, 19, INK,
                   weight="bold" if j == 3 else "normal")
    p.text(700, 388, "2.  Meter to DC volts, 2 V range. Black on GND.", 19, INK)
    p.text(700, 422, "3.  Turn the pot in small steps and read directly.", 19, INK)
    p.text(700, 456, "4.  Set it to the figure from the table.", 19, INK)
    p.text(700, 508, "Vref set  =", 20, INK, weight="bold")
    p.line(880, 514, 1240, 514, INK, 2)
    p.text(1252, 508, "V", 20, INK)
    p.wrap(700, 566,
           "The pot is sensitive — a few degrees is 0.1 V. Creep up on the number; do "
           "not sweep past it and come back.", 17, MUTE, cols=54, lh=24)
    p.wrap(700, 632,
           "0.99 V is 0.7 A RMS on an R110 module. The A4988 formula would say 0.19 V "
           "and give you about 0.14 A — a motor that skips under load and looks too "
           "small for the job.", 17, WARN, cols=54, lh=24)

    p.rect(52, 730, 1496, 2, fill=FAINT, stroke="none")
    y = p.wrap(52, 776,
               "The TMC2209 uses VREF as a SCALING input against a full-scale current fixed "
               "by the sense resistor — it is not the A4988 formula, and using that one "
               "gives about 0.14 A, at which the motor skips under load and looks "
               "inadequate. The equation these numbers come from:", 18, INK, cols=104, lh=26)
    p.mono(52, y + 36, "I_RMS  =  [ 0.325 / (R_sense + 0.02) ] / sqrt(2)  x  ( Vref / 2.5 )",
           21, INK, weight="bold")
    p.banner(52, y + 62, 1000,
             "MOTOR DISCONNECTED for this whole page. That is rule 3 on page 2: set the "
             "current before the motor can be subjected to it.", "danger", size=17)
    return p


# ============================== 18, 19 =====================================
def page18(pins, table):
    p = Page(18, "Connection 1  —  coil A", "POWER OFF while you do this")
    draw_tmc(p, hot=("1A", "1B"))
    x, y = draw_motor(p, x=1230, y=300)
    a, b = tmc_right_xy("1A"), tmc_right_xy("1B")
    run(p, [a, (1090, a[1]), (1090, y + 32), (x - 46, y + 32)], "MRED")
    run(p, [b, (1140, b[1]), (1140, y + 61), (x - 46, y + 61)], "MBLUE")
    p.text(1000, 236, "PAIR 1", 22, INK, "middle", "bold")
    p.text(1000, 266, "drawn as red + blue", 15, MUTE, "middle")
    p.text(1000, 288, "use YOUR measured pair", 15, MUTE, "middle")
    p.rect(52, 730, 1496, 2, fill=FAINT, stroke="none")
    yy = p.wrap(52, 776,
                "Both ends of ONE coil — the pair you found on page 4 — go to 1A and 1B. "
                "Which of the two ends goes to which pin does not matter; it only reverses "
                "rotation, and direction is a firmware bit. What matters is that this "
                "bridge gets both ends of one coil and never one end of each.",
                19, INK, cols=104, lh=28)
    p.banner(52, yy + 14, 1000,
             "VM MUST BE OFF. Plugging a coil into a live driver is rule 1 on page 2, and "
             "it is the most common way these modules die.", "danger", size=17)
    return p


def page19(pins, table):
    p = Page(19, "Connection 2  —  coil B", "POWER OFF while you do this")
    draw_tmc(p, hot=("2A", "2B"))
    x, y = draw_motor(p, x=1230, y=300)
    a, b = tmc_right_xy("2A"), tmc_right_xy("2B")
    # 2B is the upper pin and green the upper wire: routed so the two coil
    # leads do not cross each other on the page.
    run(p, [b, (1090, b[1]), (1090, y + 90), (x - 46, y + 90)], "MGREEN")
    run(p, [a, (1140, a[1]), (1140, y + 119), (x - 46, y + 119)], "MBLACK")
    p.text(1000, 236, "PAIR 2", 22, INK, "middle", "bold")
    p.text(1000, 266, "drawn as green + black", 15, MUTE, "middle")
    p.text(1000, 288, "the pair page 4 left over", 15, MUTE, "middle")
    p.rect(52, 730, 1496, 2, fill=FAINT, stroke="none")
    yy = p.wrap(52, 776,
                "The other coil, to 2A and 2B. Same rule: both ends of one coil into one "
                "bridge. Mixing them across the two bridges does not turn the motor — it "
                "buzzes, locks or judders, and it stresses the driver.", 19, INK,
                cols=104, lh=28)
    p.banner(52, yy + 14, 1000,
             "WITH THE MOTOR CONNECTED AND POWER STILL OFF, one last meter check: 1A to 1B "
             "beeps, 2A to 2B beeps, and 1A to 2A does NOT.", "good", size=17)
    return p


# ============================== 20 =========================================
def page20(pins, table):
    p = Page(20, "Finished layout", "What the breadboard should look like")
    draw_esp(p)
    draw_tmc(p)
    x, y = draw_motor(p, x=1230, y=150)
    draw_psu(p, "9 V")
    pairs = [("STEP", "GPIO%d" % pins["STEP"], "STEP"),
             ("DIR", "GPIO%d" % pins["DIR"], "DIR"),
             ("EN", "GPIO%d" % pins["EN"], "EN"),
             ("MS1", "3V3", "MS1"), ("MS2", "3V3", "MS2")]
    for i, (lp, ep, key) in enumerate(pairs):
        a, b = tmc_left_xy(lp), esp_xy(ep)
        run(p, [a, (470 + i * 14, a[1]), (470 + i * 14, b[1]), b], key)
    a, b = tmc_right_xy("VIO"), esp_xy("3V3")
    run(p, [a, (950, a[1]), (950, 700), (440, 700), (440, b[1]), b], "VIO")
    a, b = tmc_right_xy("GND", 1), esp_xy("GND")
    run(p, [a, (972, a[1]), (972, 716), (420, 716), (420, b[1]), b], "GNDL")
    a = tmc_right_xy("VM")
    run(p, [a, (1090, a[1]), (1090, PSU_Y + 44), (PSU_X - 22, PSU_Y + 44)], "VM")
    a = tmc_right_xy("GND", 0)
    run(p, [a, (1112, a[1]), (1112, PSU_Y + 106), (PSU_X - 22, PSU_Y + 106)], "GNDP")
    for pin, wy, key in (("1A", y + 32, "MRED"), ("1B", y + 61, "MBLUE"),
                         ("2A", y + 90, "MGREEN"), ("2B", y + 119, "MBLACK")):
        a = tmc_right_xy(pin)
        run(p, [a, (1010 + TMC_RIGHT_OFFSET(pin), a[1]),
                (1010 + TMC_RIGHT_OFFSET(pin), wy), (x - 46, wy)], key)
    p.text(52, 1000, "The geometry-proving check, once this is powered:", 20, INK, weight="bold")
    p.mono(52, 1036, "step 0 3200", 22, INK, weight="bold")
    p.wrap(300, 1036,
           "must be EXACTLY one drum revolution. Two revolutions means MS1/MS2 are giving "
           "1/8; four means 1/4. Judder with no net rotation means the coil pairing is "
           "wrong. Mark the drum first.", 17, INK, cols=76, lh=24)
    return p


def TMC_RIGHT_OFFSET(pin):
    return {"1A": 0, "1B": 22, "2A": 44, "2B": 66}[pin]


# ============================== 21 =========================================
def page21(pins, table):
    p = Page(21, "Power on, and power off", "In this order, every time")
    on = ["Everything wired, MOTOR INCLUDED, nothing powered.",
          "Confirm EN reads high (disabled) — page 7.",
          "USB-C from the PC to the ESP32.  VIO comes up; logic defined.",
          "Watch the console boot.  Board healthy BEFORE any motor voltage.",
          "maint on     — stops it hunting for a Hall that is not there.",
          "Apply VM (9 V).  Output stage now live.",
          "en 1         — and only now are the coils energised."]
    off = ["en 0        — de-energise the coils first.",
           "Remove VM.",
           "Remove USB.",
           "Before touching the motor plug: confirm the bulk cap has discharged."]
    p.text(52, 140, "POWER ON", 26, GOOD, weight="bold")
    for i, s in enumerate(on):
        yy = 180 + i * 66
        p.rect(52, yy, 720, 54, fill="#eff8f2", stroke=GOOD, sw=2, rx=6)
        p.circle(88, yy + 27, 19, GOOD)
        p.text(88, yy + 34, str(i + 1), 19, "#ffffff", "middle", "bold")
        p.text(122, yy + 34, s, 17, INK)
    p.text(830, 140, "POWER OFF", 26, DANGER, weight="bold")
    for i, s in enumerate(off):
        yy = 180 + i * 66
        p.rect(830, yy, 718, 54, fill="#fdf0ef", stroke=DANGER, sw=2, rx=6)
        p.circle(866, yy + 27, 19, DANGER)
        p.text(866, yy + 34, str(i + 1), 19, "#ffffff", "middle", "bold")
        p.text(900, yy + 34, s, 17, INK)
    y = p.banner(830, 452, 718,
                 "THE BULK CAPACITOR HOLDS CHARGE after the supply is gone. That is still a "
                 "live driver as far as rule 1 is concerned.", "danger", size=17)
    p.text(830, y + 44, "Standstill-current check (page 12)", 20, INK, weight="bold")
    p.wrap(830, y + 78,
           "With the motor energised and stationary, watch the supply current: it must STEP "
           "DOWN about a second after the last step. That is standstill reduction working, "
           "and it is the whole thermal contract. If it never steps down, PDN is tied the "
           "wrong way.", 17, INK, cols=62, lh=25)
    p.text(52, 660, "STOP NOW — cut VM immediately", 24, DANGER, weight="bold")
    stop = ["any smell of hot plastic or varnish",
            "the driver too hot to keep a finger on",
            "the motor case climbing past uncomfortable early in a run",
            "the bulk capacitor warm, or bulging",
            "loud buzzing with no rotation  (coil pairing — page 4)",
            "supply current spiking, or the supply cutting out",
            "the ESP32 rebooting repeatedly  (brownout — check grounds)"]
    for i, s in enumerate(stop):
        p.circle(66, 700 + i * 34, 6, DANGER)
        p.text(88, 706 + i * 34, s, 18, INK)
    p.wrap(52, 960,
           "Do not go looking for the console if something smells hot. Pull VM.",
           19, DANGER, cols=70, lh=26, weight="bold")
    p.wrap(830, 960,
           "EXPECTED, not a problem: faults on columns 1-4 (nothing is wired to them), a "
           "no_hall fault on column 0 before `maint on`, a steady hiss at standstill (the "
           "chopper), and the drum settling to a slightly different rest position after "
           "`en 0` — that is the 3.92 N·cm imbalance against a 2.2 N·cm detent, and seeing "
           "it confirms the premise.", 16, MUTE, cols=64, lh=23)
    return p


PAGES = [page01, page02, page03, page04]


def all_pages(pins, table):
    out = [f(pins, table) for f in PAGES]
    out += make_conn_pages(pins, table)
    out += [page15(pins, table), page16(pins, table), page17(pins, table),
            page18(pins, table), page19(pins, table), page20(pins, table),
            page21(pins, table)]
    out.sort(key=lambda p: p.num)
    return out
