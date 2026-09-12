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
    TMC_LEFT, TMC_RIGHT,
    BB_ROWS, BB_X, BB_Y, BB_PITCH, BB_COLS_TOP, BB_COLS_BOT, BB_CHANNEL,
    bb_size, bb_channel_y, wrap_h,
    DRIVER_ROW0, DRIVER_COL_L, DRIVER_COL_R,
    JUMPER_COL_L, JUMPER_COL_R, SUPPLY_COL, CAP_COL, COIL_COL,
    bb_xy, draw_breadboard, draw_bb_module, driver_row, jumper_hole,
    board_dest, jumper_counts,
)

# The one sentence every physical page carries, because the distinction it draws
# is the whole reason the electrical pages can be trusted and these cannot be
# trusted the same way.  Kept in one place so the pages cannot paraphrase it
# differently from each other.
LAYOUT_CAVEAT = ("ROW AND COLUMN NUMBERS ARE LAYOUT, NOT CHECKED FACTS. The pins "
                 "on this page are parsed from hal/pins.h and BENCH_WIRING.md and "
                 "cross-checked; no source says which HOLE anything sits in, so "
                 "nothing verifies that. Build it elsewhere on the board and the "
                 "electrical pages are still true while these numbers are not.")


# --------------------------------------------------------------------------
# THE PAGE ORDER, AND WHY IT IS A REGISTRY RATHER THAN LITERAL NUMBERS.
# The pages cross-reference each other constantly - "rule 3 on page 2", "the
# pair you found on page 4".  Level 0 inserted seven pages in the middle of
# that, and hand-renumbering a dozen references is exactly the kind of edit
# that silently points a reader at the wrong sheet.  So a page has a NAME, its
# number is its position in this list, and prose says P("vref") rather than 17.
PAGE_ORDER = [
    "bom", "bomprep",                       # level 0: what you need
    "cover", "killers", "headers",          # rules, and the one solder job
    "parts", "board", "coilpairs",          # identify, the board, the coils
    "c_step", "c_dir", "c_en", "c_vio", "c_gndl",
    "c_ms1", "c_ms2", "c_pdn", "c_vm", "c_gndp",
    "cap", "physlogic", "physpower",        # the bulk cap, then the real board
    "continuity", "vref", "coila", "coilb",
    "finished", "physdone", "power",
]
PNUM = {k: i + 1 for i, k in enumerate(PAGE_ORDER)}


def P(key):
    return PNUM[key]


def board_strip(p, key, drv, dest, which=0, x=1070, y=752, w=478):
    """The physical landing for one connection, on the connection's own page."""
    p.rect(x, y, w, 250, fill="#f7f9fb", stroke=FAINT, sw=2, rx=8)
    p.text(x + 20, y + 34, "ON THE BOARD", 18, INK, weight="bold")
    p.line(x + 20, y + 46, x + w - 20, y + 46, FAINT, 2)
    yy = p.wrap(x + 20, y + 78, board_dest(key, drv, dest, which), 16, INK,
                cols=44, lh=23)
    p.wrap(x + 20, yy + 16, LAYOUT_CAVEAT, 12, MUTE, cols=58, lh=17)


def conn_page(num, cnum, drv, dest, key, what, why, warn=None, warn_kind="danger",
              done=(), volts="9 V", extra=None, board_key=None, esp_pin=None):
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
        p.text(1260, 452, "then VERIFY on page %d" % P("power"), 17, INK, "middle", "bold")
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
    if board_key:
        board_strip(p, board_key, hot[0], esp_pin, gnd if hot[0] == "GND" else 0)
    y = p.wrap(52, 776, why, 19, INK, cols=70, lh=28)
    if warn:
        p.banner(52, y + 16, 990, warn, warn_kind, size=17)
    if extra:
        extra(p)
    return p


# ============================== 1 ==========================================
def page01(pins, table):
    p = Page(P("cover"), "Bench wiring — module V1", "First power-on, connection by connection")
    y = p.wrap(52, 148,
               "The whole job in %d pages, in the order you should do it. One connection "
               "per page. Every pin number here is read out of hal/pins.h and "
               "docs/BENCH_WIRING.md when these pages are rendered, so a drawing cannot "
               "quietly disagree with the firmware. Row and column numbers are a different "
               "kind of statement - page %d says which." % (len(PAGE_ORDER), P("bomprep")),
               19, INK, cols=64, lh=28)
    y = p.banner(52, y + 14, 720,
                 "The Hall sensor and magnet are NOT fitted. No homing, no position, no "
                 "closed loop this round. That is expected, not a fault.", "warn")
    y = p.banner(52, y + 8, 720,
                 "READ PAGE %d FIRST. The three rules on it are how TMC2209 drivers die."
                 % P("killers"), "danger")
    p.text(52, y + 52, "THE ORDER, and why it is this order", 20, INK, weight="bold")
    # Two columns, because SVG collapses runs of spaces and a padded mono
    # string comes out ragged.
    def rng(a, b):
        return "%d-%d" % (P(a), P(b)) if P(a) != P(b) else str(P(a))

    steps = [(rng("bom", "bomprep"), "collect the parts, and prepare three of them", 0),
             (rng("cover", "headers"), "the rules, and the one solder job (skipped)", 0),
             (rng("parts", "coilpairs"), "identify the parts, the board, the coil pairs", 0),
             (rng("c_step", "c_gndl"), "wire the logic  (driver to ESP32)", 0),
             (rng("c_ms1", "c_gndp"), "wire the microsteps, then the power", 0),
             (rng("cap", "physpower"), "the bulk capacitor, then the board as BUILT", 0),
             (rng("continuity", "continuity"), "continuity check  <-  find mistakes with a meter", 1),
             (rng("vref", "vref"), "set Vref  <-  motor still NOT connected", 1),
             (rng("coila", "coilb"), "connect the motor  <-  power off while you do it", 1),
             (rng("finished", "power"), "finished layout, the built board, power on / off", 0)]
    for i, (num, txt, strong) in enumerate(steps):
        yy = y + 82 + i * 28
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
    p = Page(P("killers"), "Three ways to kill the driver", "Read before you pick anything up")
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
         "on pages %d-%d, in that order, deliberately." % (P("coila"), P("coilb"))),
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
           "breadboard rail at the far end of the board. Page %d shows where." % P("cap"),
           18, INK, cols=98, lh=26)
    p.wrap(52, y + 146,
           "If you are unsure at any point: power off, and nothing you do next can break "
           "anything. Every irreversible mistake in this guide needs VM to be live.",
           17, GOOD, cols=104, lh=25)
    return p


# ============================== 3 ==========================================
def page03(pins, table):
    p = Page(P("parts"), "Identify your two parts", "Match by LABEL — drawn positions are only a hint")
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
               "R150 or R100. That marking decides your Vref target on page %d, and reading " % P("vref") +
               "it takes ten seconds where guessing it costs you the whole thermal result.",
               18, INK, cols=94, lh=26)
    p.rect(52, y + 12, 420, 44, fill="none", stroke=INK, sw=2, rx=4)
    p.mono(68, y + 42, "R _____   =   0. _____ ohm", 19, MUTE)
    p.text(560, y + 42, "R110 = 0.11 ohm  ·  R150 = 0.15 ohm  ·  R100 = 0.10 ohm",
           17, MUTE)
    return p


# ============================== 4 ==========================================
def page04(pins, table):
    p = Page(P("coilpairs"), "Find the coil pairs", "Motor connected to NOTHING. Meter only.")
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
    p.text(52, y2 + 34, "Write it down — pages %d and %d refer to it:" % (P("coila"), P("coilb")), 19, INK, weight="bold")
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

    def add(pkey, cnum, drv, dest, key, what, why, warn=None, wk="danger",
            extra=None):
        out.append(conn_page(P(pkey), cnum, drv, dest, key, what, why, warn, wk,
                             done=tuple(done), extra=extra, board_key=key, esp_pin=dest))
        done.append(drv if isinstance(drv, str) else drv[0])

    add("c_step", "3", "STEP", "esp:" + g % pins["STEP"], "STEP",
        "STEP  ->  " + g % pins["STEP"],
        "One pulse on this wire moves the motor one microstep. At 1/16 microstepping "
        "that is 1/64th of a flap, and 3200 pulses is exactly one drum revolution. This "
        "is the only wire that carries motion.")

    add("c_dir", "4", "DIR", "esp:" + g % pins["DIR"], "DIR",
        "DIR  ->  " + g % pins["DIR"],
        "A level, not a pulse: the driver reads it on each STEP edge to decide which way "
        "to turn. The ring is descending, so getting this backwards makes a countdown "
        "count up — which is why it is a firmware bit you can flip with `dir` rather than "
        "something to get right with a soldering iron.")

    add("c_en", "5", "EN", "esp:" + g % pins["EN"], "EN",
        "EN  ->  " + g % pins["EN"],
        "ENABLE, and it is ACTIVE LOW: pulling it low turns the coils on. Most modules "
        "pull it high on board, so an unconnected EN means disabled — which is the safe "
        "default you want for a first power-on.",
        "CHECK YOURS. With USB in and VM off, measure EN against GND: it should read "
        "about 3.3 V. If it reads near 0 V, add a 10 kohm resistor from EN to 3V3 before "
        "you ever connect VM, or the coils energise the instant power arrives.", "warn")

    add("c_vio", "6", "VIO", "esp-around", "VIO",
        "VIO  ->  3V3",
        "The logic supply. It sets the voltage the driver expects to see on STEP, DIR, EN "
        "and the MS pins, so it must be the SAME 3.3 V the ESP32 drives with. VIO is on "
        "the right-hand pin column, so this one routes under the module.",
        "3.3 V, not 5 V. Feeding VIO 5 V makes the driver want 5 V logic levels from a "
        "3.3 V microcontroller. It appears to work, right up until it does not.", "danger")

    add("c_gndl", "7", "GND", "esp-around", "GNDL",
        "GND (logic)  ->  GND",
        "The logic ground. Without a shared ground the ESP32 and the driver have no common "
        "reference and every signal level is meaningless. This is the wire whose absence "
        "produces the most baffling symptoms.")

    add("c_ms1", "10", "MS1", "esp3v3", "MS1",
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

    add("c_ms2", "11", "MS2", "esp3v3", "MS2",
        "MS2  ->  3V3",
        "Microstep select, bit two. Tie it HIGH as well. BOTH high is 1/16 on a TMC2209, "
        "and the table is not in a sensible order — which is exactly why this one is easy "
        "to get wrong.",
        "Modules usually pull MS1 and MS2 LOW on board. Leave them unconnected and you get "
        "1/8, which makes every constant in the firmware wrong by a factor of two. Page %d "
        "has a mechanical check that proves the setting — do not trust the jumper." % P("finished"),
        "warn",
        extra=ms_table)

    add("c_pdn", "12", "PDN", "none", "PDN",
        "PDN / UART  —  standstill current",
        "In standalone mode this pin controls automatic standstill current reduction: about "
        "a second after the last step, the driver drops the coil current to roughly half. "
        "That is not a nicety here — it is the entire thermal contract, because the motor "
        "is sealed in a PLA drum and holds current all day.",
        "THE POLARITY IS NOT VERIFIED for your module and this guide will not guess it. "
        "Leave PDN as the module ships for standalone, then confirm on page %d: with the " % P("power") + 
        "motor energised and still, the supply current must visibly STEP DOWN about a "
        "second after the last step. If it never does, tie the pin the other way and "
        "re-check.", "warn")

    add("c_vm", "9", "VM", "psu", "VM",
        "VM  ->  supply  +",
        "The motor supply. Start at 9 V from the PD trigger, not 20 V: the TMC2209 runs "
        "from 4.75 V up, at one drum revolution per second there is no headroom needed, "
        "and a wiring mistake at 9 V dissipates about a fifth of the energy. Move to 20 V "
        "once the wiring is proven.")

    add("c_gndp", "8", "GND", "psu", "GNDP",
        "GND (power)  ->  supply  -",
        "The power ground — the wire that carries the coil current back. Keep it short and "
        "direct. This is a different job from the logic ground on page %d even though they " % P("c_gndl") + 
        "are the same net; on a breadboard, route it separately and let them meet at the "
        "supply.")
    return out


# ============================== 15 =========================================
def page15(pins, table):
    p = Page(P("cap"), "Bulk capacitor  —  connection 13", "100 uF, at the driver's OWN pins")
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
    y = p.banner(52, y + 16, 1000,
                 "ON A BREADBOARD THIS MEANS THE TWO HOLES BESIDE THE MODULE, not the "
                 "power rails at the end of the board. Twenty centimetres of breadboard "
                 "wire defeats the capacitor entirely. This is the single thing breadboard "
                 "builds get wrong.", "danger", size=17)
    p.text(52, y + 34, "Concretely, on this build:", 18, INK, weight="bold")
    p.mono(340, y + 34, "+ leg row %d column %s    -  leg row %d column %s"
           % (driver_row("VM")[0], CAP_COL, driver_row("GND", 0)[0], CAP_COL), 19, INK,
           weight="bold")
    p.text(52, y + 62, "drawn on page %d; lead prep on page %d"
           % (P("physpower"), P("bomprep")), 16, MUTE)
    return p


# ============================== 16 =========================================
def page16(pins, table):
    p = Page(P("continuity"), "Continuity check", "Beep these five pairs BEFORE the first power-on")
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
           "wiring error. Go to page %d." % P("vref"), 18, GOOD, cols=116, lh=26)
    return p


# ============================== 17 =========================================
def page17(pins, table):
    v011, f011 = tmc2209_vref(0.7, 0.11)
    v015, f015 = tmc2209_vref(0.7, 0.15)
    # The subtitle is the guide's own precondition sentence, verbatim.  It used
    # to read "VM ON", which BENCH_WIRING.md section 4 step 2 flatly contradicts.
    p = Page(P("vref"), "Set Vref  —  0.7 A", FACTS["vref_pre"])
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
             "MOTOR DISCONNECTED for this whole page. That is rule 3 on page %d: set the " % P("killers") + 
             "current before the motor can be subjected to it.", "danger", size=17)
    return p


# ============================== 18, 19 =====================================
def page18(pins, table):
    p = Page(P("coila"), "Connection 1  —  coil A", "POWER OFF while you do this")
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
                "Both ends of ONE coil — the pair you found on page %d — go to 1A and 1B. " % P("coilpairs") + 
                "Which of the two ends goes to which pin does not matter; it only reverses "
                "rotation, and direction is a firmware bit. What matters is that this "
                "bridge gets both ends of one coil and never one end of each.",
                19, INK, cols=104, lh=28)
    p.banner(52, yy + 14, 1000,
             "VM MUST BE OFF. Plugging a coil into a live driver is rule 1 on page %d, and " % P("killers") + 
             "it is the most common way these modules die.", "danger", size=17)
    return p


def page19(pins, table):
    p = Page(P("coilb"), "Connection 2  —  coil B", "POWER OFF while you do this")
    draw_tmc(p, hot=("2A", "2B"))
    x, y = draw_motor(p, x=1230, y=300)
    a, b = tmc_right_xy("2A"), tmc_right_xy("2B")
    # 2B is the upper pin and green the upper wire: routed so the two coil
    # leads do not cross each other on the page.
    run(p, [b, (1090, b[1]), (1090, y + 90), (x - 46, y + 90)], "MGREEN")
    run(p, [a, (1140, a[1]), (1140, y + 119), (x - 46, y + 119)], "MBLACK")
    p.text(1000, 236, "PAIR 2", 22, INK, "middle", "bold")
    p.text(1000, 266, "drawn as green + black", 15, MUTE, "middle")
    p.text(1000, 288, "the pair page %d left over" % P("coilpairs"), 15, MUTE, "middle")
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
    p = Page(P("finished"), "Finished layout",
             "Every connection at once - the ELECTRICAL truth.  Page %d is the board itself."
             % P("physdone"))
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
    p.text(52, 944, "The geometry-proving check, once this is powered:", 20, INK,
           weight="bold")
    p.mono(52, 980, "step 0 3200", 22, INK, weight="bold")
    p.wrap(300, 980,
           "must be EXACTLY one drum revolution. Two revolutions means MS1/MS2 are giving "
           "1/8; four means 1/4. Judder with no net rotation means the coil pairing is "
           "wrong. Mark the drum first.", 17, INK, cols=76, lh=24)
    return p


def TMC_RIGHT_OFFSET(pin):
    return {"1A": 0, "1B": 22, "2A": 44, "2B": 66}[pin]


# ============================== 21 =========================================
def page21(pins, table):
    p = Page(P("power"), "Power on, and power off", "In this order, every time")
    on = ["Everything wired, MOTOR INCLUDED, nothing powered.",
          "USB-C from the PC to the ESP32.  VIO comes up; logic defined.",
          "Confirm EN reads high (disabled) — page %d.  USB in, VM still off." % P("c_en"),
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
    p.text(830, y + 44, "Standstill-current check (page %d)" % P("c_pdn"), 20, INK, weight="bold")
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
            "loud buzzing with no rotation  (coil pairing — page %d)" % P("coilpairs"),
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


# ======================= LEVEL 0:  WHAT YOU NEED ===========================
def page_bom(pins, table):
    p = Page(P("bom"), "Bench bill of materials",
             "Every physical object the session needs. Tick it, then start.")
    jc = jumper_counts(table)
    p.banner(52, 108, 1496,
             "NOTHING HERE NEEDS ACQUIRING. Every item was confirmed on hand on "
             "2026-09-11, the 830-point breadboard included. This box exists so that "
             "anything a later revision adds lands on page 1 rather than being "
             "discovered on page 13.", "good", size=16)
    items = [
        ("TMC2209 StepStick (FYSETC)", "1 of 6",
         "The driver. Headers arrived PRE-SOLDERED, confirmed by opening the box, so page "
         "%d is a skip. All six are one revision, so one Vref carries to all five later."
         % P("headers")),
        ("ESP32-C5-DevKitC-1", "1",
         "Already on the bench. It stays OFF the breadboard - page %d says why - so every "
         "wire that reaches it is male-to-female." % P("board")),
        ("NEMA 17 + printed stand-in axle", "1",
         "The load. The axle is PLA, which is why the firmware clamps every commanded "
         "speed to one drum revolution per second and why that cap is not configurable."),
        ("Breadboard, full-size 830-point", "1",
         "63 rows, two rail pairs, an 0.3 in centre channel. The channel is the only "
         "reason a StepStick works on a breadboard at all - page %d." % P("board")),
        ("Jumper wires, male-to-FEMALE", str(jc["mf"]),
         "Three signals (STEP, DIR, EN) and the two rail feeds. Female onto the ESP32's "
         "pins, male into the board."),
        ("Jumper wires, male-to-male", str(jc["mm"]),
         "Board-internal only: VIO, GND (logic), MS1 and MS2 to the rails. Both counts "
         "are derived from the parsed connection table, not typed on this page."),
        ("JST-XH 4-way mating pigtail", "1",
         "THE MOTOR PLUG CANNOT ENTER A BREADBOARD. The pigtail's four flying leads can. "
         "Page %d shows exactly where they terminate." % P("bomprep")),
        ("100 uF electrolytic, 35 V or more", "1",
         "The bulk capacitor. Its legs need trimming and bending before it will reach the "
         "right two holes - lead prep on page %d." % P("bomprep")),
        ("Hookup wire, 22 AWG SOLID core", "30 cm red + black",
         "RotoPD screw terminal to the board. Solid, not stranded: stranded frays in a "
         "breadboard hole, will not hold, and leaves a strand behind."),
        ("Flat screwdriver, 2 mm blade", "1",
         "Two jobs: the RotoPD's screw terminals, and the Vref trimpot on page %d. A blade "
         "that does not fit the pot slips off it onto a pad." % P("vref")),
        ("Multimeter with a continuity beep", "1",
         "Coil pairs (page %d), continuity (page %d), Vref (page %d). Continuity and DC "
         "volts on a 2 V range are the only functions used."
         % (P("coilpairs"), P("continuity"), P("vref"))),
        ("USB-C cable, PC to ESP32", "1",
         "Console and logic power. A DATA cable - a charge-only lead gives a board that "
         "powers up, looks alive, and never appears as a serial port."),
        ("USB-C PD cable + RotoPD trigger", "1",
         "VM. 9 V while the wiring is being proven, then 20 V before the soak, from the "
         "same trigger board."),
        ("Hex key for the drum set screw", "1",
         "Fit the stand-in drum, and MARK it - a tape flag will do - so that `step 0 3200` "
         "is a readable result rather than a guess."),
        ("Heatsink (supplied, loose)", "0  -  NOT this session",
         "Deliberately not installed. Page %d has the reasoning and the one condition that "
         "would change it." % P("bomprep")),
        ("Hall sensor + magnet", "0  -  NOT this session",
         "Branch B: no Hall fitted, so no homing, no edge figures, no revs. That is the "
         "expected configuration for this session and not a fault."),
    ]
    for i, (name, qty, why) in enumerate(items):
        cx = 52 if i < 8 else 812
        yy = 236 + (i % 8) * 100
        p.rect(cx, yy, 736, 92, fill="#fafbfc", stroke=FAINT, sw=2, rx=6)
        p.rect(cx + 16, yy + 16, 26, 26, fill="none", stroke=INK, sw=2, rx=4)
        p.text(cx + 58, yy + 36, name, 17, INK, weight="bold")
        p.mono(cx + 720, yy + 36, qty, 15, MUTE, "end")
        p.wrap(cx + 58, yy + 62, why, 13, MUTE, cols=84, lh=18)
    return p


def page_bomprep(pins, table):
    p = Page(P("bomprep"), "What the schematic does not say",
             "Three parts need preparing, one decision is already made, and one thing is "
             "deliberately NOT checked")
    cap_plus = (driver_row("VM")[0], CAP_COL)
    cap_minus = (driver_row("GND", 0)[0], CAP_COL)
    coils = [(n, driver_row(n)[0]) for n in ("2B", "2A", "1A", "1B")]
    LC, LW = 84, 15          # left column: characters per line, text size
    RC = 78                  # right column

    def box(x, y, w, h, n, head):
        p.rect(x, y, w, h, fill="#ffffff", stroke=INK, sw=2, rx=8)
        p.rect(x, y, w, 44, fill="#eef2f6", stroke="none", rx=8)
        p.rect(x, y + 36, w, 8, fill="#eef2f6", stroke="none")
        p.circle(x + 28, y + 22, 16, INK)
        p.text(x + 28, y + 28, n, 16, "#ffffff", "middle", "bold")
        p.text(x + 54, y + 29, head, 19, INK, weight="bold")
        return y + 44

    t1 = ("The loom ends in a 4-way JST-XH. Its shell will not go into an 0.1 in board and "
          "forcing it spreads the contacts permanently. The mating pigtail plugs onto it "
          "and ends in four bare leads: strip 6 mm, twist each hard or tin it, and push "
          "them into rows %d to %d of column %s. WHICH lead goes where is decided by the "
          "meter on page %d, never by colour - these wires were cut and re-terminated."
          % (coils[0][1], coils[-1][1], COIL_COL, P("coilpairs")))
    t2a = ("Strip 8 mm of the 22 AWG solid core, clamp it under the screw, and run the "
           "other end straight into the driver's own row. VM never goes through a rail.")
    t2b = ("BELIEVED to be screw terminals; the unit is unopened. If yours is not, STOP "
           "AND REPORT - do not improvise a connector on a 20 V rail.")
    t3 = ("It bridges the driver's OWN VM and GND, which are adjacent rows - 2.54 mm "
          "apart, against the 3.5 or 5 mm the leads came at. Trim both to about 12 mm, "
          "bend them toward each other at the body, and check the bends cannot touch. The "
          "body may overhang other rows; only the legs matter. Backwards, it vents.")

    h1 = 44 + 124 + wrap_h(t1, LW, LC, 21) + 20
    h2 = 44 + 132 + wrap_h(t2a, LW, LC, 21) + 10 + wrap_h(t2b, LW, LC, 21) + 20
    h3 = 44 + 132 + wrap_h(t3, LW, LC, 21) + 20

    # 1 - the pigtail
    by = box(52, 104, 760, h1, "1", "The motor plug cannot enter a breadboard")
    p.text(120, by + 18, "on the motor", 13, MUTE, "middle")
    p.text(218, by + 18, "pigtail", 13, MUTE, "middle")
    p.rect(84, by + 26, 72, 44, fill="#f0f2f5", stroke=INK, sw=2, rx=4)
    p.text(120, by + 53, "JST-XH", 13, INK, "middle", "bold")
    p.rect(196, by + 32, 44, 32, fill="#d7dce1", stroke=INK, sw=2, rx=3)
    for i, (nm, row) in enumerate(coils):
        _n, col, _sw = COLOURS[("MGREEN", "MBLACK", "MRED", "MBLUE")[i]]
        p.poly([(240, by + 38 + i * 7), (288, by + 26 + i * 22), (318, by + 26 + i * 22)],
               stroke=col, sw=5)
        p.mono(328, by + 32 + i * 22, "%s  ->  row %d col %s" % (nm, row, COIL_COL),
               15, INK)
    p.wrap(84, by + 138, t1, LW, INK, cols=LC, lh=21)

    # 2 - the RotoPD
    y2 = 104 + h1 + 14
    by = box(52, y2, 760, h2, "2", "The RotoPD reaches the board on two wires")
    p.text(248, by + 24, "screw terminals", 14, MUTE)
    p.rect(84, by + 30, 152, 66, fill="#f0f2f5", stroke=INK, sw=2, rx=4)
    for i in range(2):
        p.rect(102 + i * 66, by + 40, 46, 36, fill="#c7ced6", stroke=INK, sw=2, rx=3)
        p.circle(125 + i * 66, by + 58, 11, "#9aa4ae", INK, 2)
        p.line(118 + i * 66, by + 58, 132 + i * 66, by + 58, INK, 3)
    p.text(125, by + 90, "+", 18, "#d0342c", "middle", "bold")
    p.text(191, by + 92, "-", 22, "#22262b", "middle", "bold")
    run(p, [(125, by + 40), (125, by + 8), (400, by + 8)], "VM")
    run(p, [(191, by + 76), (191, by + 116), (400, by + 116)], "GNDP")
    p.mono(412, by + 14, "row %d col %s" % (driver_row("VM")[0], SUPPLY_COL), 16, INK)
    p.mono(412, by + 122, "row %d col %s" % (driver_row("GND", 0)[0], SUPPLY_COL), 16, INK)
    y2b = p.wrap(84, by + 152, t2a, LW, INK, cols=LC, lh=21)
    p.wrap(84, y2b + 10, t2b, LW, WARN, cols=LC, lh=21)

    # 3 - the capacitor
    y3 = y2 + h2 + 14
    by = box(52, y3, 760, h3, "3", "The capacitor needs its legs prepared")
    p.text(130, by + 20, "stripe = negative", 13, MUTE, "middle")
    p.rect(97, by + 28, 66, 84, fill="#f0f2f5", stroke=INK, sw=3, rx=9)
    p.rect(145, by + 31, 15, 78, fill="#c7ced6", stroke="none", rx=4)
    p.text(120, by + 64, "100", 18, INK, "middle", "bold")
    p.text(120, by + 88, "uF", 15, INK, "middle")
    p.line(112, by + 112, 112, by + 140, INK, 4)
    p.line(140, by + 112, 140, by + 140, INK, 4)
    p.text(96, by + 136, "+", 18, "#d0342c", "middle", "bold")
    p.text(156, by + 138, "-", 22, "#22262b", "middle", "bold")
    p.mono(210, by + 62, "+   row %d col %s" % cap_plus, 16, INK)
    p.mono(210, by + 92, "-   row %d col %s" % cap_minus, 16, INK)
    p.wrap(84, by + 158, t3, LW, INK, cols=LC, lh=21)

    # 4 - the heatsink
    t4 = ("DECIDED: the driver runs bare. 0.7 A RMS against the module's 2 A rating is "
          "roughly an eighth of rated dissipation, and the soak's thermal question is the "
          "motor sealed in PLA, not the driver. Page %d's STOP NOW list already carries "
          "'the driver too hot to keep a finger on', so this is watched rather than "
          "assumed." % P("power"))
    t4b = ("Not independently verified: the exact RDS(on)-derived junction temperature. IF "
           "one ever goes on, the adhesive pad MUST clear the Vref trimpot - a heatsink "
           "overhanging the pot ends page %d." % P("vref"))
    h4 = 44 + wrap_h(t4, 15, RC, 21) + 14 + wrap_h(t4b, 14, RC + 4, 19) + 22
    by = box(840, 104, 708, h4, "4", "Heatsink: not needed this session")
    y4 = p.wrap(864, by + 34, t4, 15, INK, cols=RC, lh=21)
    p.wrap(864, y4 + 14, t4b, 14, WARN, cols=RC + 4, lh=19)

    # 5 - the current path
    t5 = ("0.7 A RMS through 22 AWG wire, stock jumpers and breadboard rails is fine and "
          "needs no thought. A breadboard contact is good for about 1 A continuous and a "
          "jumper drops tens of millivolts here. Two things are worth KNOWING rather than "
          "fixing: the 0.7 A coil current flows in the four motor leads and inside the "
          "driver, never along a rail; and VM and GND come in on their own wires to the "
          "driver's own rows to keep the switching return out of the logic ground, not for "
          "current rating. The bulk capacitor, not the supply wire, delivers each step's "
          "transient. Upgrade nothing.")
    h5 = 44 + wrap_h(t5, 15, RC, 21) + 22
    y5 = 104 + h4 + 14
    by = box(840, y5, 708, h5, "5", "The current path, so nobody over-engineers")
    p.wrap(864, by + 34, t5, 15, INK, cols=RC, lh=21)

    # 6 - the guard's scope
    t6 = ("Every PIN on every page is parsed out of hal/pins.h and docs/BENCH_WIRING.md, "
          "cross-checked against the other, and the generator emits nothing at all if they "
          "disagree. That is why a drawing here cannot quietly contradict the firmware.")
    t6c = ("It is the same distinction page %d draws between a LABEL and a drawn position. "
           "The coordinates are chosen in tools/wiringgen.py, they are consistent with each "
           "other, and consistent is not verified." % P("parts"))
    h6 = (44 + wrap_h(t6, 15, RC, 21) + 16 + wrap_h(LAYOUT_CAVEAT, 15, RC, 21)
          + 14 + wrap_h(t6c, 14, RC + 4, 19) + 22)
    y6 = y5 + h5 + 14
    by = box(840, y6, 708, h6, "6", "What the guard checks, and what it cannot")
    ya = p.wrap(864, by + 34, t6, 15, INK, cols=RC, lh=21)
    yb = p.wrap(864, ya + 16, LAYOUT_CAVEAT, 15, DANGER, cols=RC, lh=21)
    p.wrap(864, yb + 14, t6c, 14, MUTE, cols=RC + 4, lh=19)
    return p


def page_headers(pins, table):
    p = Page(P("headers"), "Solder the headers",
             "SKIPPED this session - the modules arrived with headers already on")
    y = p.banner(52, 108, 1496,
                 "SKIP THIS PAGE AND GO TO PAGE %d. The FYSETC modules arrived "
                 "PRE-SOLDERED - confirmed by opening the box, not inferred from a product "
                 "photo. The page stays because the next module to arrive may not be, and "
                 "because the orientation below is the one thing that cannot be undone."
                 % P("parts"), "good")
    p.text(52, y + 52, "Confirm it in five seconds before you move on", 21, INK,
           weight="bold")
    checks = ["Two black strips of 8 pins each are IN the module, not loose in the bag.",
              "Every pin has a shiny cone of solder where it meets the board.",
              "Sighted from the end, all sixteen pins are parallel and the same length.",
              "The pins point AWAY from the components, i.e. out of the underside."]
    for i, c in enumerate(checks):
        yy = y + 92 + i * 40
        p.rect(56, yy - 20, 26, 26, fill="none", stroke=INK, sw=2, rx=4)
        p.text(98, yy, c, 17, INK)

    ybase = y + 268
    p.text(52, ybase, "If a future module arrives with loose strips", 21, INK,
           weight="bold")
    p.wrap(52, ybase + 36,
           "Solder it STRADDLED INTO THE BREADBOARD. The board holds all sixteen pins "
           "square and at the right spacing while you work, which is the whole problem - a "
           "module soldered freehand comes out with splayed pins that will not enter a "
           "breadboard, and straightening them afterwards breaks them at the joint.",
           17, INK, cols=100, lh=25)

    py = ybase + 160
    for x, label, ok in ((52, "RIGHT  -  pins held by the board", True),
                         (836, "WRONG  -  pins held by hand or tape", False)):
        col = GOOD if ok else DANGER
        p.rect(x, py, 660, 330, fill="#eff8f2" if ok else "#fdf0ef", stroke=col, sw=3, rx=8)
        p.text(x + 24, py + 40, label, 21, col, weight="bold")

    # A SIDE elevation: the board at the bottom, two header strips standing in
    # it, the module resting on their tops.  Seen from above this drawing says
    # nothing, and what has to be got right here is up and down.
    fx = 52
    p.poly([(fx + 330, py + 68), (fx + 330, py + 96)], stroke=INK, sw=3)
    p.poly([(fx + 322, py + 88), (fx + 330, py + 96), (fx + 338, py + 88)], stroke=INK, sw=3)
    p.text(fx + 352, py + 84, "solder from THIS side", 16, GOOD, weight="bold")
    p.rect(fx + 140, py + 104, 360, 22, fill="#3c6e47", stroke=INK, sw=2, rx=3)
    p.text(fx + 320, py + 120, "module, component side UP", 14, "#ffffff", "middle", "bold")
    p.rect(fx + 70, py + 168, 470, 76, fill="#fbfbf9", stroke=INK, sw=2, rx=5)
    p.rect(fx + 78, py + 198, 454, 14, fill="#eceff2", stroke="none", rx=2)
    p.rect(fx + 190, py + 126, 16, 78, fill="#2b3138", stroke=INK, sw=1, rx=2)
    p.rect(fx + 434, py + 126, 16, 78, fill="#2b3138", stroke=INK, sw=1, rx=2)
    p.mono(fx + 320, py + 236, "0.6 in", 14, MUTE, "middle")
    p.wrap(fx + 24, py + 276,
           "Strips into the board pins-down, module dropped on top, solder the sixteen "
           "joints you can see.", 15, INK, cols=74, lh=20)

    gx = 836
    p.rect(gx + 90, py + 94, 430, 46, fill="#3c6e47", stroke=INK, sw=2, rx=4)
    p.text(gx + 305, py + 124, "module", 15, "#ffffff", "middle", "bold")
    for i, dx in enumerate((0, 9, -7, 4, -3, 11, -10, 6)):
        p.line(gx + 120 + i * 52, py + 140, gx + 120 + i * 52 + dx, py + 198, "#2b3138", 7)
    p.text(gx + 305, py + 234, "splayed - will not enter a board", 17, DANGER, "middle",
           "bold")
    p.wrap(gx + 24, py + 276,
           "Sixteen pins cannot be held square by hand. This is the failure that costs a "
           "module rather than a minute.", 15, INK, cols=74, lh=20)
    return p


# ======================= LEVEL 0:  THE BOARD ================================
DRV_ROWS = tuple(range(DRIVER_ROW0, DRIVER_ROW0 + len(TMC_LEFT)))
BW, BH = bb_size()


def _board(p, y0, hot=DRV_ROWS, label_every=5):
    draw_breadboard(p, hot_rows=hot, y0=y0, label_every=label_every)
    draw_bb_module(p, DRIVER_ROW0, len(TMC_LEFT), DRIVER_COL_L, DRIVER_COL_R,
                   "TMC2209", y0=y0)


def _row_labels(p, y0, y):
    p.mono(bb_xy(DRV_ROWS[0], "A", y0)[0] - 46, y, "row", 14, MUTE, "end")
    for r in DRV_ROWS:
        p.mono(bb_xy(r, "A", y0)[0], y, str(r), 15, INK, "middle", "bold")


def page_board(pins, table):
    p = Page(P("board"), "The breadboard, and where the driver sits",
             "Nothing is wired yet. This is the board, drawn as it is.")
    y0 = 250
    p.wrap(52, 130,
           "Two facts about this board do all the work. Each group of five holes across ONE "
           "row, on one side of the channel, is ONE electrical node - so a wire in row %d "
           "column %s is electrically the driver's VM pin. And the four long strips along "
           "the edges are the POWER RAILS, which are joined to nothing at all until page %d "
           "makes them live." % (driver_row("VM")[0], CAP_COL, P("physlogic")),
           18, INK, cols=110, lh=26)
    _board(p, y0)
    _row_labels(p, y0, y0 + BH + 26)
    cx, _cy = bb_xy(DRIVER_ROW0, "J", y0)
    p.poly([(cx - 44, y0 + 152), (cx - 104, y0 + 112)], stroke=INK, sw=2)
    p.rect(cx - 404, y0 + 90, 300, 24, fill="#fbfbf9", stroke="none")
    p.text(cx - 110, y0 + 106, "the module straddles the channel", 16, INK, "end", "bold")
    p.text(1210, bb_channel_y(y0) + 5,
           "0.3 in channel  -  the only reason a StepStick works on a breadboard",
           14, MUTE, "middle")

    y = y0 + BH + 64
    p.text(52, y + 30, "Why the driver lands in columns %s and %s"
           % (DRIVER_COL_L, DRIVER_COL_R), 20, INK, weight="bold")
    p.wrap(52, y + 66,
           "A StepStick's two headers are 0.6 in apart. Counting across the channel, "
           "column %s to column %s is 0.1+0.1+0.1+0.3+0.1+0.1 = 0.6 in exactly, so the "
           "module drops in with no pin sharing a node with its opposite number. That "
           "spacing is the one certain physical fact these coordinates are built on."
           % (DRIVER_COL_L, DRIVER_COL_R), 16, INK, cols=86, lh=23)
    p.banner(52, y + 176, 740,
             "COLUMNS E, F AND G DO NOT EXIST for this build: the module body sits over "
             "them. Every wire on the right half goes into I or %s, every wire on the left "
             "half into %s. That is clearance, not tidiness." % (CAP_COL, JUMPER_COL_L),
             "warn", size=16)

    p.text(840, y + 30, "THE ESP32 STAYS OFF THE BOARD", 20, INK, weight="bold")
    p.wrap(840, y + 66,
           "Its header-to-header spacing is 0.9 in or 1.0 in depending on the inset, and "
           "on an 0.1 in board with an 0.3 in channel both of them fit - landing the pins "
           "in different columns. Which GPIO sits where along the header is a board fact "
           "this repository does not carry, and guessing bends pins. So the DevKitC-1 lies "
           "loose on the bench and every wire to it is male-to-FEMALE.",
           16, INK, cols=86, lh=23)
    p.text(840, y + 226, "Find these five by label on the silkscreen:", 17, INK,
           weight="bold")
    labs = ["GPIO%d  (STEP)" % pins["STEP"], "GPIO%d  (DIR)" % pins["DIR"],
            "GPIO%d  (EN)" % pins["EN"], "3V3", "GND"]
    for i, lab in enumerate(labs):
        lx, ly = 844 + (i % 3) * 240, y + 248 + (i // 3) * 38
        p.rect(lx, ly, 24, 24, fill="none", stroke=INK, sw=2, rx=4)
        p.mono(lx + 32, ly + 18, lab, 16, INK)
    p.wrap(840, y + 330, LAYOUT_CAVEAT, 13, MUTE, cols=92, lh=18)
    return p


def _phys_rows(table):
    """The logic connections, in the guide's own row order, with their holes."""
    out = []
    for num in sorted(table):
        drv, esp = table[num]
        which = 1 if (drv == "GND" and esp == "GND") else 0
        drow, dcol = driver_row(drv, which)
        jrow, jcol = jumper_hole(drv, which)
        if esp.startswith("GPIO"):
            to, kind, key = "ESP  " + esp, "M-F", drv
        elif esp == "3V3":
            to, kind, key = "+ rail", "M-M", drv
        else:
            to, kind, key = "- rail", "M-M", "GNDL"
        name = "GND (logic)" if which else drv
        out.append((name, drow, dcol, jrow, jcol, to, kind, key))
    return out


def page_physlogic(pins, table):
    p = Page(P("physlogic"), "The board as built - logic side",
             "The same connections as pages %d to %d, by row and column"
             % (P("c_step"), P("c_gndl")))
    y0 = 190
    rows = _phys_rows(table)
    _board(p, y0)
    _row_labels(p, y0, y0 + BH + 28)
    top_plus, top_minus = y0 + 28, y0 + 48
    bot_plus, bot_minus = y0 + BH - 48, y0 + BH - 28

    esc = 0
    for name, _dr, _dc, jrow, jcol, to, _kind, key in rows:
        hx, hy = bb_xy(jrow, jcol, y0)
        if to in ("+ rail", "- rail"):
            up = jcol in BB_COLS_TOP
            ry = (top_plus if to == "+ rail" else top_minus) if up else \
                 (bot_plus if to == "+ rail" else bot_minus)
            run(p, [(hx, hy), (hx, ry)], key)
        else:
            # A left-half jumper leaves across the bottom rails, because that is
            # exactly what it does on the bench: it lies over them and off the edge.
            ly = hy + 16 + esc * 7
            lx = 200 + esc * 22
            run(p, [(hx, hy), (hx, ly), (lx, ly), (lx, 700)], key)
            esc += 1
    for nm in ("PDN", "CLK"):
        ex, ey = bb_xy(*jumper_hole(nm), y0=y0)
        p.circle(ex, ey, 9, "none", DANGER, 3)
        p.line(ex - 6, ey - 6, ex + 6, ey + 6, DANGER, 3)

    p.rect(150, 700, 320, 96, fill="#f0f2f5", stroke=INK, sw=3, rx=8)
    p.text(310, 738, "ESP32-C5-DevKitC-1", 17, INK, "middle", "bold")
    p.text(310, 764, "loose on the bench", 15, MUTE, "middle")
    run(p, [(220, 796), (220, 836), (124, 836), (124, bot_plus)], "VIO")
    run(p, [(300, 796), (300, 864), (100, 864), (100, bot_minus)], "GNDL")
    p.mono(490, 842, "3V3  ->  + rail", 15, INK)
    p.mono(490, 870, "GND  ->  - rail", 15, INK)
    p.wrap(150, 916,
           "Every wire that leaves the board for the ESP32 crosses the bottom rails on its "
           "way out. That is what a jumper does; it lies on top of them and touches "
           "nothing.", 15, MUTE, cols=54, lh=21)

    tx, ty = 860, 640
    p.text(tx, ty, "Every wire, by hole", 20, INK, weight="bold")
    colx = [0, 150, 300, 450, 610]
    for j, hc in enumerate(("pin", "pin is at", "wire goes in", "and reaches", "")):
        p.text(tx + colx[j], ty + 32, hc, 15, MUTE, weight="bold")
    yy = ty + 62
    for name, dr, dc, jrow, jcol, to, kind, _key in rows:
        p.mono(tx, yy, name, 16, INK, weight="bold")
        p.mono(tx + colx[1], yy, "%d %s" % (dr, dc), 16, INK)
        p.mono(tx + colx[2], yy, "%d %s" % (jrow, jcol), 16, INK)
        p.mono(tx + colx[3], yy, to, 16, INK)
        p.mono(tx + colx[4], yy, kind, 14, MUTE)
        yy += 30
    for nm, to in (("3V3 feed", "+ rail"), ("GND feed", "- rail")):
        p.mono(tx, yy, nm, 16, INK, weight="bold")
        p.mono(tx + colx[2], yy, "a rail", 16, INK)
        p.mono(tx + colx[3], yy, to, 16, INK)
        p.mono(tx + colx[4], yy, "M-F", 14, MUTE)
        yy += 30
    pr, pc = jumper_hole("PDN")
    p.mono(tx, yy + 8, "PDN, CLK", 16, DANGER, weight="bold")
    p.mono(tx + colx[1], yy + 8, "%d, %d %s" % (pr, jumper_hole("CLK")[0], pc), 16, DANGER)
    p.mono(tx + colx[2], yy + 8, "NOTHING", 16, DANGER, weight="bold")
    p.mono(tx + colx[3], yy + 8, "stay empty", 16, DANGER)
    p.wrap(tx, yy + 54, LAYOUT_CAVEAT, 13, MUTE, cols=90, lh=18)
    return p


def page_physpower(pins, table):
    p = Page(P("physpower"), "The board as built - power side",
             "The supply, the capacitor and the motor, by row and column")
    y0 = 260
    vm_row = driver_row("VM")[0]
    gnd_row = driver_row("GND", 0)[0]
    coils = [(n, driver_row(n)[0]) for n in ("2B", "2A", "1A", "1B")]
    _board(p, y0)
    _row_labels(p, y0, y0 + BH + 28)

    # NOTHING CROSSES ANYTHING HERE, and that is worth the trouble: a crossing
    # on a page somebody reads at a live 20 V bench is a wire they put in the
    # wrong row.  Rows 26 and 27 each carry two occupants in the top half - a
    # supply wire in %s and a capacitor leg in %s - so the two bundles are given
    # separate lanes out of the board rather than a tidy-looking junction.
    p.wrap(1252, 122,
           "All six wires leave over the top edge and lie flat on the board. "
           "Nothing here is soldered: every one is a push fit and comes out "
           "again with your fingers.", 15, MUTE, cols=35, lh=21)

    # the bulk capacitor: legs into the two CAP_COL holes, body above the rails
    ax, ay = bb_xy(vm_row, CAP_COL, y0)
    bx, by = bb_xy(gnd_row, CAP_COL, y0)
    p.line(ax, ay, ax, ay - 44, "#d0342c", 5)
    p.line(bx, by, bx, by - 44, "#22262b", 5)
    p.rect(ax - 20, ay - 86, (bx - ax) + 40, 42, fill="#f0f2f5", stroke=INK, sw=3, rx=8)
    p.rect(bx + 2, ay - 83, 14, 36, fill="#c7ced6", stroke="none", rx=4)
    p.text((ax + bx) / 2, ay - 58, "100 uF", 13, INK, "middle", "bold")
    p.text(ax - 32, ay - 14, "+", 18, "#d0342c", "middle", "bold")
    p.text(bx + 34, ay - 12, "-", 22, "#22262b", "middle", "bold")
    p.text(130, 236, "the stripe marks the - leg", 14, MUTE)

    # the supply pair: GND takes the inner lane, VM the outer, so the two
    # diagonals never meet and neither one passes under the capacitor.
    sx, sy = bb_xy(vm_row, SUPPLY_COL, y0)
    gx, gy = bb_xy(gnd_row, SUPPLY_COL, y0)
    run(p, [(gx, gy), (630, 320), (630, 200)], "GNDP")
    run(p, [(sx, sy), (560, 300), (560, 150)], "VM")
    p.text(642, 190, "RotoPD  -", 16, INK, weight="bold")
    p.text(548, 144, "RotoPD  +      9 V first, 20 V before the soak", 16, INK, "end",
           weight="bold")

    # the four coil leads: the leftmost takes the highest lane, so no lead's
    # horizontal ever crosses another lead's vertical.
    for i, (nm, row) in enumerate(coils):
        cx2, cy2 = bb_xy(row, COIL_COL, y0)
        key = ("MGREEN", "MBLACK", "MRED", "MBLUE")[i]
        ly = 168 + i * 26
        run(p, [(cx2, cy2), (cx2, ly), (880, ly)], key)
        p.mono(892, ly + 6, "%s   ->   one motor lead" % nm, 15, INK)
    p.text(892, 118, "which lead is which is decided by", 14, MUTE)
    p.text(892, 138, "the meter on page %d, never by colour" % P("coilpairs"), 14, MUTE)

    y = y0 + BH + 66
    p.text(52, y + 26, "Every power wire, by hole", 20, INK, weight="bold")
    lines = [("VM  (driver)", "%d %s" % (vm_row, DRIVER_COL_R),
              "RotoPD  +", "%d %s" % (vm_row, SUPPLY_COL), "22 AWG"),
             ("GND  (power)", "%d %s" % (gnd_row, DRIVER_COL_R),
              "RotoPD  -", "%d %s" % (gnd_row, SUPPLY_COL), "22 AWG"),
             ("100 uF  +", "-", "bridges VM", "%d %s" % (vm_row, CAP_COL), "legs 12 mm"),
             ("100 uF  -", "-", "bridges GND", "%d %s" % (gnd_row, CAP_COL), "stripe side")]
    for nm, row in coils:
        lines.append(("%s  (coil)" % nm, "%d %s" % (row, DRIVER_COL_R),
                      "pigtail lead", "%d %s" % (row, COIL_COL), "meter decides"))
    colx = [0, 200, 330, 500, 660]
    for j, hc in enumerate(("what", "pin is at", "other end", "wire goes in", "")):
        p.text(52 + colx[j], y + 58, hc, 15, MUTE, weight="bold")
    for i, cells in enumerate(lines):
        yy = y + 88 + i * 30
        for j, cell in enumerate(cells):
            p.mono(52 + colx[j], yy, cell, 16 if j < 4 else 14,
                   INK if j < 4 else MUTE, weight="bold" if j == 0 else "normal")

    p.banner(920, y + 26, 628,
             "THE CAPACITOR IS NOT OPTIONAL AND ITS POSITION IS THE POINT. Rows %d and %d "
             "ARE the driver's VM and GND pins, which is why it belongs there and not on a "
             "rail twenty centimetres away." % (vm_row, gnd_row), "danger", size=16)
    p.wrap(920, y + 172,
           "0.7 A RMS needs none of this upgraded - page %d says why. The coil current "
           "never travels along a rail: it runs in the four leads above and inside the "
           "driver. The supply wires are separate from the logic ground to keep the "
           "switching return out of it, not for current rating."
           % P("bomprep"), 15, INK, cols=72, lh=21)
    p.wrap(920, y + 302, LAYOUT_CAVEAT, 13, MUTE, cols=82, lh=18)
    return p


def page_physdone(pins, table):
    p = Page(P("physdone"), "The finished board",
             "Walk rows %d to %d once, before anything is powered"
             % (DRV_ROWS[0], DRV_ROWS[-1]))
    y0 = 250
    rows = _phys_rows(table)
    vm_row = driver_row("VM")[0]
    gnd_row = driver_row("GND", 0)[0]
    coils = [(n, driver_row(n)[0]) for n in ("2B", "2A", "1A", "1B")]
    _board(p, y0)
    _row_labels(p, y0, y0 + BH + 26)

    occupied = {}
    for name, _dr, _dc, jrow, jcol, _to, _k, _key in rows:
        occupied[(jrow, jcol)] = name
    occupied[(vm_row, SUPPLY_COL)] = "VM in"
    occupied[(gnd_row, SUPPLY_COL)] = "GND in"
    occupied[(vm_row, CAP_COL)] = "cap +"
    occupied[(gnd_row, CAP_COL)] = "cap -"
    for nm, row in coils:
        occupied[(row, COIL_COL)] = nm
    # Dots, not labels: at 0.1 in pitch a name per hole is unreadable, and the
    # row-by-row list below says what each one is anyway.
    for (r, c), _label in sorted(occupied.items()):
        hx, hy = bb_xy(r, c, y0)
        p.circle(hx, hy, 9, GOOD)
    lx = bb_xy(DRV_ROWS[-1] + 2, "J", y0)[0]
    for col, what in ((CAP_COL, "capacitor, coils, VIO, GND (logic)"),
                      (SUPPLY_COL, "the two RotoPD supply wires"),
                      (JUMPER_COL_L, "STEP, DIR, EN, MS1, MS2")):
        _hx, hy = bb_xy(1, col, y0)
        p.line(lx, hy, lx + 26, hy, GOOD, 2)
        p.rect(lx + 30, hy - 12, 400, 24, fill="#fbfbf9", stroke="none")
        p.mono(lx + 34, hy + 5, "%s   %s" % (col, what), 14, INK)

    y = y0 + BH + 64
    p.text(52, y + 26, "The whole board, row by row", 20, INK, weight="bold")
    per_row = {}
    for (r, c), label in occupied.items():
        per_row.setdefault(r, []).append((c, label))
    for i, r in enumerate(DRV_ROWS):
        yy = y + 66 + i * 40
        left = TMC_LEFT[r - DRIVER_ROW0]
        right = TMC_RIGHT[r - DRIVER_ROW0]
        here = ", ".join("%s %s" % (c, l) for c, l in sorted(per_row.get(r, [])))
        p.rect(52, yy - 24, 1000, 34, fill="#fafbfc" if i % 2 else "#ffffff",
               stroke="none")
        p.rect(56, yy - 20, 24, 24, fill="none", stroke=INK, sw=2, rx=4)
        p.mono(96, yy, "row %d" % r, 16, INK, weight="bold")
        p.mono(196, yy, "%-8s | %s" % (left, right), 16, MUTE)
        p.mono(520, yy, here if here else "nothing - and that is correct", 16,
               INK if here else DANGER)

    p.text(1090, y + 26, "Count before you power anything", 20, INK, weight="bold")
    jc = jumper_counts(table)
    tally = [("jumper wires", jc["total"]), ("supply wires from the RotoPD", 2),
             ("capacitor legs", 2), ("motor leads from the pigtail", len(coils))]
    for i, (what, n) in enumerate(tally):
        p.mono(1090, y + 66 + i * 32, "%2d" % n, 19, INK, weight="bold")
        p.text(1134, y + 66 + i * 32, what, 16, INK)
    p.line(1090, y + 182, 1500, y + 182, FAINT, 2)
    p.mono(1090, y + 210, "%2d" % (jc["total"] + 4 + len(coils)), 19, INK, weight="bold")
    p.text(1134, y + 210, "things in the board, total", 16, INK, weight="bold")
    p.banner(1090, y + 234, 458,
             "A hole you cannot account for is a wire in the wrong row. Pull it out and "
             "find it on page %d or %d." % (P("physlogic"), P("physpower")), "warn",
             size=15)
    p.wrap(1090, y + 348,
           "Then the meter, page %d. It finds what a count cannot: two wires in one row "
           "that should not share it." % P("continuity"), 15, GOOD, cols=58, lh=21)
    return p



PAGES = [page_bom, page_bomprep, page01, page02, page_headers, page03,
         page_board, page04]


def all_pages(pins, table):
    out = [f(pins, table) for f in PAGES]
    out += make_conn_pages(pins, table)
    out += [page15(pins, table), page_physlogic(pins, table),
            page_physpower(pins, table), page16(pins, table), page17(pins, table),
            page18(pins, table), page19(pins, table), page20(pins, table),
            page_physdone(pins, table), page21(pins, table)]
    out.sort(key=lambda p: p.num)
    # The registry is the only place an order is written down; this catches a
    # page function that was added to PAGE_ORDER and never called, or twice.
    if len(out) != len(PAGE_ORDER):
        raise SystemExit("wiringgen: PAGE_ORDER has %d entries, %d pages were built"
                         % (len(PAGE_ORDER), len(out)))
    return out
