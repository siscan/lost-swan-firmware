#!/usr/bin/env python3
"""Render docs/wiring/ - the illustrated bench wiring guide.

WHY THIS IS A GENERATOR AND NOT A SET OF DRAWINGS.

A picture of a wiring loom is a claim about GPIO numbers, and a picture cannot
be code-reviewed.  Hand-drawn diagrams in an embedded repo go stale silently:
the pin map moves, the drawing does not, and the drawing is the thing somebody
follows at a bench with a soldering iron.  So every pin number on every page is
READ FROM THE SOURCE at render time:

  * GPIO numbers come from components/swan_hal/include/hal/pins.h, parsed out
    of the DevKitC-1 block.
  * The driver-pin <-> ESP-pin mapping comes from the connection table in
    docs/BENCH_WIRING.md, parsed out of the markdown.
  * The two are CROSS-CHECKED against each other, and the Vref figures in the
    prose are re-derived from the TMC2209 current equation and checked too.

If any of those disagree the tool prints what and raises, emitting nothing.
`--check` runs the whole verification without rendering and is wired into
test-host.ps1 and CI, so a pin-map change that would falsify a picture fails
the build instead of reaching a bench.

Usage:
    python tools/wiringgen.py            # verify, then write docs/wiring/*.svg
    python tools/wiringgen.py --check    # verify only, no output
"""

import argparse
import math
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
PINS_H = ROOT / "components" / "swan_hal" / "include" / "hal" / "pins.h"
GUIDE = ROOT / "docs" / "BENCH_WIRING.md"
OUT = ROOT / "docs" / "wiring"

W, H = 1600, 1131
INK, MUTE, FAINT, PAPER, BAND = "#14181d", "#6b7683", "#c7ced6", "#ffffff", "#eef1f4"
DANGER, GOOD, WARN = "#c0261b", "#1a7f45", "#b8791a"

FSANS = "DejaVu Sans, Verdana, Geneva, sans-serif"
FMONO = "DejaVu Sans Mono, Consolas, monospace"


# ===========================================================================
# 1.  READ THE SOURCES, AND REFUSE TO DRAW IF THEY DISAGREE
# ===========================================================================
def parse_pins_h(text):
    start = text.index("#if defined(BOARD_ESP32C5_DEVKITC1)")
    end = text.index("#else  // BOARD_XIAO_ESP32C5")
    blk = text[start:end]

    def scalar(name):
        m = re.search(r"inline constexpr int " + name + r"\s*=\s*(-?\d+)\s*;", blk)
        if not m:
            raise SystemExit("wiringgen: %s missing from the DevKitC-1 block" % name)
        return int(m.group(1))

    def array0(name):
        m = re.search(r"inline constexpr int " + name + r"\[N_COLUMNS\]\s*=\s*\{\s*(-?\d+)", blk)
        if not m:
            raise SystemExit("wiringgen: %s[0] missing from the DevKitC-1 block" % name)
        return int(m.group(1))

    return {"STEP": array0("PIN_STEP"), "HALL": array0("PIN_HALL"),
            "EN": scalar("PIN_EN"), "DIR": scalar("PIN_DIR")}


def parse_guide_table(text):
    rows = {}
    pat = re.compile(r"^\|\s*(\d+)\s*\|\s*`([^`]+)`[^|]*\|[^|]*\|\s*\*\*([^*]+)\*\*\s*\|", re.M)
    for m in pat.finditer(text):
        rows[int(m.group(1))] = (m.group(2).strip(), m.group(3).strip())
    if not rows:
        raise SystemExit("wiringgen: no connection rows parsed from BENCH_WIRING.md")
    return rows


def parse_guide_vref(text):
    out = {}
    for m in re.finditer(r"\|\s*\*\*(0\.\d+)[^|]*\|\s*([\d.]+)\s*A\s*\|\s*\*\*([\d.]+)\s*V\*\*",
                         text):
        out[float(m.group(1))] = (float(m.group(2)), float(m.group(3)))
    return out


# Prose facts quoted onto the pages, filled by verify().  A page that states
# one of these states the guide's own words rather than a second copy of them.
FACTS = {}


def parse_guide_ms_table(text):
    i = text.index("| MS2 | MS1 | microsteps |")
    blk = text[i:text.index("\n\n", i)]
    rows, want = [], None
    for ln in blk.splitlines():
        m = re.match(r"\|\s*\**(\d)\**\s*\|\s*\**(\d)\**\s*\|\s*\**(1/\d+)\**", ln)
        if m:
            rows.append((int(m.group(1)), int(m.group(2)), m.group(3)))
            if "what we want" in ln:
                want = rows[-1]
    return rows, want


def parse_guide_vref_pre(text):
    i = text.index("### Step 1 ")
    m = re.search(r"\*\*([^*]*\bVM\b[^*]*)\*\*", text[i:])
    return m.group(1).strip() if m else ""


def tmc2209_vref(i_rms, r_sense, v_fs=0.325):
    """The TMC2209 relationship.  NOT the A4988 one - see the 2026-09-11 log."""
    full_scale = (v_fs / (r_sense + 0.02)) / math.sqrt(2.0)
    return 2.5 * i_rms / full_scale, full_scale


def verify(loud=False):
    pins = parse_pins_h(PINS_H.read_text(encoding="utf-8"))
    gtxt = GUIDE.read_text(encoding="utf-8")
    table = parse_guide_table(gtxt)
    bad = []

    want = {"STEP": pins["STEP"], "DIR": pins["DIR"], "EN": pins["EN"]}
    seen = {}
    for num, (drv, esp) in table.items():
        key = drv.split()[0].upper()
        if key in want:
            m = re.match(r"GPIO(\d+)$", esp)
            if not m:
                bad.append("row %d: %s -> %r is not a GPIO" % (num, drv, esp))
            else:
                seen[key] = int(m.group(1))
    for k, v in want.items():
        if k not in seen:
            bad.append("BENCH_WIRING.md has no row for %s" % k)
        elif seen[k] != v:
            bad.append("%s: pins.h says GPIO%d, the guide says GPIO%d" % (k, v, seen[k]))

    for num, (drv, esp) in table.items():
        u = drv.upper()
        if u.startswith("VIO") and esp != "3V3":
            bad.append("VIO must go to 3V3; the guide says %r" % esp)
        if u.startswith("MS") and esp != "3V3":
            bad.append("%s must be tied to 3V3 for 1/16; the guide says %r" % (drv, esp))

    ms = sorted(d.upper() for _n, (d, _e) in table.items() if d.upper().startswith("MS"))
    if ms != ["MS1", "MS2"]:
        bad.append("expected MS1 and MS2 rows, found %r" % ms)

    ms_rows, ms_want = parse_guide_ms_table(gtxt)
    if [(a, b) for a, b, _r in ms_rows] != [(0, 0), (0, 1), (1, 0), (1, 1)]:
        bad.append("the MS table in BENCH_WIRING.md is not the four MS2/MS1 rows")
    elif ms_want is None or ms_want[2] != "1/16":
        bad.append("the MS row marked 'what we want' is %r, not 1/16" % (ms_want,))

    pre = parse_guide_vref_pre(gtxt)
    if not pre:
        bad.append("no Vref precondition sentence found in BENCH_WIRING.md section 4")
    elif "disconnect" not in pre.lower():
        bad.append("the Vref precondition %r does not say the motor is disconnected, "
                   "which page 2 rule 3 and page 17 both claim" % pre)

    vrefs = parse_guide_vref(gtxt)
    if not vrefs:
        bad.append("no Vref table parsed from BENCH_WIRING.md")
    for r_sense, (full_doc, vref_doc) in vrefs.items():
        vref, full = tmc2209_vref(0.7, r_sense)
        if abs(vref - vref_doc) > 0.02:
            bad.append("Vref @ %.2f ohm: computed %.2f V, guide says %.2f V"
                       % (r_sense, vref, vref_doc))
        if abs(full - full_doc) > 0.02:
            bad.append("full scale @ %.2f ohm: computed %.2f A, guide says %.2f A"
                       % (r_sense, full, full_doc))

    if bad:
        sys.stderr.write("wiringgen: the drawings would disagree with the source.\n")
        for b in bad:
            sys.stderr.write("  - %s\n" % b)
        raise SystemExit(1)

    FACTS.update(ms=ms_rows, vref_pre=pre)

    if loud:
        print("wiringgen: pins.h and BENCH_WIRING.md agree")
        print("  STEP=GPIO%(STEP)d  DIR=GPIO%(DIR)d  EN=GPIO%(EN)d" % pins)
        print("  %d connection rows, %d Vref rows, all checked" % (len(table), len(vrefs)))
        print("  microstep table %s" % (", ".join("%d%d=%s" % r for r in ms_rows)))
        print("  Vref precondition quoted: %r" % pre)
    return pins, table


# ===========================================================================
# 2.  SVG
# ===========================================================================
def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


class Page:
    def __init__(self, num, title, subtitle=""):
        self.num, self.title, self.subtitle, self.b = num, title, subtitle, []

    def rect(self, x, y, w, h, fill="none", stroke=INK, sw=2, rx=0, dash=None):
        d = ' stroke-dasharray="%s"' % dash if dash else ""
        self.b.append('<rect x="%g" y="%g" width="%g" height="%g" rx="%g" fill="%s" '
                      'stroke="%s" stroke-width="%g"%s/>' % (x, y, w, h, rx, fill, stroke, sw, d))

    def line(self, x1, y1, x2, y2, stroke=INK, sw=2, dash=None):
        d = ' stroke-dasharray="%s"' % dash if dash else ""
        self.b.append('<line x1="%g" y1="%g" x2="%g" y2="%g" stroke="%s" stroke-width="%g" '
                      'stroke-linecap="round"%s/>' % (x1, y1, x2, y2, stroke, sw, d))

    def poly(self, pts, stroke=INK, sw=2, fill="none", dash=None):
        d = ' stroke-dasharray="%s"' % dash if dash else ""
        s = " ".join("%g,%g" % xy for xy in pts)
        self.b.append('<polyline points="%s" fill="%s" stroke="%s" stroke-width="%g" '
                      'stroke-linecap="round" stroke-linejoin="round"%s/>' % (s, fill, stroke, sw, d))

    def circle(self, cx, cy, r, fill=INK, stroke="none", sw=1):
        self.b.append('<circle cx="%g" cy="%g" r="%g" fill="%s" stroke="%s" '
                      'stroke-width="%g"/>' % (cx, cy, r, fill, stroke, sw))

    def text(self, x, y, s, size=18, fill=INK, anchor="start", weight="normal", family=FSANS):
        self.b.append('<text x="%g" y="%g" font-family="%s" font-size="%g" fill="%s" '
                      'text-anchor="%s" font-weight="%s">%s</text>'
                      % (x, y, family, size, fill, anchor, weight, esc(s)))

    def mono(self, x, y, s, size=17, fill=INK, anchor="start", weight="normal"):
        self.text(x, y, s, size, fill, anchor, weight, FMONO)

    def wrap(self, x, y, s, size=17, fill=INK, cols=70, lh=25, weight="normal"):
        line, n = [], 0
        for w in s.split():
            if sum(len(t) for t in line) + len(line) + len(w) > cols and line:
                self.text(x, y + n * lh, " ".join(line), size, fill, weight=weight)
                line, n = [w], n + 1
            else:
                line.append(w)
        if line:
            self.text(x, y + n * lh, " ".join(line), size, fill, weight=weight)
            n += 1
        return y + n * lh

    def banner(self, x, y, w, s, kind="danger", size=18):
        col = {"danger": DANGER, "good": GOOD, "warn": WARN}[kind]
        bg = {"danger": "#fdf0ef", "good": "#eff8f2", "warn": "#fdf7ec"}[kind]
        # 0.62 em/char is measured against DejaVu Sans bold, which is what
        # actually renders here; 0.55 overflowed the box on seven pages.
        cols = max(8, int((w - 56) / (size * 0.62)))
        lines, line = [], []
        for word in s.split():
            if sum(len(t) for t in line) + len(line) + len(word) > cols and line:
                lines.append(" ".join(line)); line = [word]
            else:
                line.append(word)
        if line:
            lines.append(" ".join(line))
        h = 24 + len(lines) * (size + 8)
        self.rect(x, y, w, h, fill=bg, stroke=col, sw=3, rx=6)
        self.rect(x, y, 9, h, fill=col, stroke="none")
        for i, ln in enumerate(lines):
            self.text(x + 28, y + 32 + i * (size + 8), ln, size, col, weight="bold")
        return y + h + 8

    def svg(self, total=21):
        o = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
             'viewBox="0 0 %d %d">' % (W, H, W, H),
             '<rect width="%d" height="%d" fill="%s"/>' % (W, H, PAPER),
             '<rect x="0" y="0" width="%d" height="88" fill="%s"/>' % (W, BAND),
             '<line x1="0" y1="88" x2="%d" y2="88" stroke="%s" stroke-width="2"/>' % (W, FAINT)]
        hdr = Page(0, "")
        hdr.text(52, 47, self.title, 29, INK, weight="bold")
        if self.subtitle:
            hdr.text(52, 74, self.subtitle, 17, MUTE)
        hdr.text(W - 52, 52, "%d / %d" % (self.num, total), 26, MUTE, anchor="end",
                 weight="bold")
        ftr = Page(0, "")
        ftr.line(52, H - 44, W - 52, H - 44, FAINT, 1)
        ftr.text(52, H - 20, "LOST Swan  module V1  ·  generated by tools/wiringgen.py "
                             "from hal/pins.h + docs/BENCH_WIRING.md", 13, MUTE)
        return "\n".join(o + hdr.b + self.b + ftr.b + ["</svg>"])


# ===========================================================================
# 3.  THE PARTS
# ===========================================================================
TMC_LEFT = ["EN", "MS1", "MS2", "PDN", "CLK", "STEP", "DIR", "(n/c)"]
TMC_RIGHT = ["VM", "GND", "2B", "2A", "1A", "1B", "VIO", "GND"]
TMC_X, TMC_Y, TMC_W, TMC_H = 640, 250, 250, 400
ESP_X, ESP_Y, ESP_W, ESP_H = 110, 250, 260, 430
ESP_PINS = ["3V3", "GND", "GPIO6", "GPIO8", "GPIO24"]
MOT_X, MOT_Y = 1240, 170
PSU_X, PSU_Y, PSU_W, PSU_H = 1170, 520, 310, 170

COLOURS = {
    "VM":   ("RED, thick",    "#d0342c", 11),
    "GNDP": ("BLACK, thick",  "#22262b", 11),
    "VIO":  ("red, thin",     "#d0342c", 6),
    "GNDL": ("black, thin",   "#22262b", 6),
    "STEP": ("orange",        "#e07b16", 7),
    "DIR":  ("yellow",        "#c9a227", 7),
    "EN":   ("white / grey",  "#7b8794", 7),
    "MS1":  ("violet",        "#7d4bbf", 7),
    "MS2":  ("grey-blue",     "#4a7fa5", 7),
    "PDN":  ("brown",         "#8a5a2b", 7),
    # The four coil runs are drawn in the motor's OWN wire colours.  Drawing a
    # coil lead black where it lands on the wire labelled "blue" is exactly the
    # kind of thing somebody follows at a bench.
    "MRED":   ("motor red",   "#d0342c", 9),
    "MBLUE":  ("motor blue",  "#2f61c4", 9),
    "MGREEN": ("motor green", "#1f8a4c", 9),
    "MBLACK": ("motor black", "#22262b", 9),
}
MOTOR_WIRES = [("red", "#d0342c"), ("blue", "#2f61c4"),
               ("green", "#1f8a4c"), ("black", "#22262b")]


def tmc_left_xy(name):
    i = TMC_LEFT.index(name)
    return TMC_X - 22, TMC_Y + 44 + i * 42


def tmc_right_xy(name, which=0):
    idx = [i for i, n in enumerate(TMC_RIGHT) if n == name][which]
    return TMC_X + TMC_W + 22, TMC_Y + 44 + idx * 42


def esp_xy(name):
    return ESP_X + ESP_W + 22, ESP_Y + 96 + ESP_PINS.index(name) * 62


def _hot_sets(hot):
    """`hot` entries are "NAME" or ("NAME", which).  A bare "GND" would light
    BOTH ground pins, which is exactly the ambiguity pages 9 and 14 exist to
    remove, so the caller says which one."""
    left, right = set(), set()
    for h in hot:
        name, which = (h, None) if isinstance(h, str) else h
        if name in TMC_LEFT:
            left.add(TMC_LEFT.index(name))
            continue
        idx = [i for i, n in enumerate(TMC_RIGHT) if n == name]
        if not idx:
            raise SystemExit("wiringgen: no TMC pin named %r" % name)
        right.update(idx if which is None else [idx[which]])
    return left, right


def draw_tmc(p, hot=()):
    hot_l, hot_r = _hot_sets(hot)
    p.rect(TMC_X, TMC_Y, TMC_W, TMC_H, fill="#f6f8fa", stroke=INK, sw=3, rx=6)
    p.text(TMC_X + TMC_W / 2, TMC_Y - 46, "FYSETC TMC2209", 21, INK, "middle", "bold")
    p.text(TMC_X + TMC_W / 2, TMC_Y - 24, "standalone  ·  match by LABEL", 14, MUTE, "middle")
    p.rect(TMC_X + 80, TMC_Y + 150, 90, 90, fill="#2b3138", rx=4, stroke="none")
    p.text(TMC_X + TMC_W / 2, TMC_Y + 202, "TMC", 15, "#ffffff", "middle", "bold")
    p.circle(TMC_X + TMC_W / 2, TMC_Y + 305, 21, "#dfe4ea", INK, 2)
    p.line(TMC_X + TMC_W / 2 - 13, TMC_Y + 305, TMC_X + TMC_W / 2 + 13, TMC_Y + 305, INK, 4)
    p.text(TMC_X + TMC_W / 2, TMC_Y + 348, "Vref pot", 13, MUTE, "middle")
    for i, n in enumerate(TMC_LEFT):
        y = TMC_Y + 44 + i * 42
        on = i in hot_l
        p.rect(TMC_X - 18, y - 10, 18, 20, fill=INK if on else "#9aa4ae", stroke="none")
        p.mono(TMC_X + 12, y + 6, n, 15, INK if on else MUTE, weight="bold" if on else "normal")
    for i, n in enumerate(TMC_RIGHT):
        y = TMC_Y + 44 + i * 42
        on = i in hot_r
        p.rect(TMC_X + TMC_W, y - 10, 18, 20, fill=INK if on else "#9aa4ae", stroke="none")
        p.mono(TMC_X + TMC_W - 12, y + 6, n, 15, INK if on else MUTE, "end",
               "bold" if on else "normal")


def draw_esp(p, hot=()):
    p.rect(ESP_X, ESP_Y, ESP_W, ESP_H, fill="#f6f8fa", stroke=INK, sw=3, rx=6)
    p.text(ESP_X + ESP_W / 2, ESP_Y - 46, "ESP32-C5-DevKitC-1", 21, INK, "middle", "bold")
    p.text(ESP_X + ESP_W / 2, ESP_Y - 24, "USB-C at the top", 14, MUTE, "middle")
    p.rect(ESP_X + 104, ESP_Y - 16, 74, 20, fill="#9aa4ae", stroke=INK, sw=2, rx=4)
    p.text(ESP_X + ESP_W / 2, ESP_Y - 1, "USB-C", 12, INK, "middle")
    p.rect(ESP_X + 48, ESP_Y + 40, 150, 84, fill="#2b3138", rx=4, stroke="none")
    p.text(ESP_X + ESP_W / 2, ESP_Y + 90, "ESP32-C5", 15, "#ffffff", "middle", "bold")
    for i, n in enumerate(ESP_PINS):
        y = ESP_Y + 96 + i * 62
        on = n in hot
        p.rect(ESP_X + ESP_W, y - 11, 18, 22, fill=INK if on else "#9aa4ae", stroke="none")
        p.mono(ESP_X + ESP_W - 12, y + 6, n, 17, INK if on else MUTE, "end",
               "bold" if on else "normal")
    p.text(ESP_X + 16, ESP_Y + ESP_H - 16, "other pins omitted", 13, FAINT)


def draw_motor(p, x=None, y=None, hot=()):
    x = MOT_X if x is None else x
    y = MOT_Y if y is None else y
    p.rect(x, y, 150, 150, fill="#e9edf1", stroke=INK, sw=3, rx=8)
    p.circle(x + 75, y + 75, 28, "#c7ced6", INK, 2)
    p.circle(x + 75, y + 75, 9, INK)
    p.text(x + 75, y - 20, "LDO 42STH48-2504AH", 17, INK, "middle", "bold")
    for i, (nm, col) in enumerate(MOTOR_WIRES):
        wy = y + 32 + i * 29
        on = nm in hot
        p.line(x - 46, wy, x, wy, col, 8 if on else 5)
        # ABOVE the stub, not to the left of it: a coil run arrives along this
        # line and a label to its left is drawn straight through.
        p.mono(x - 6, wy - 8, nm, 13, col, "end", "bold" if on else "normal")
    return x, y


def draw_psu(p, volts="9 V"):
    p.rect(PSU_X, PSU_Y, PSU_W, PSU_H, fill="#f6f8fa", stroke=INK, sw=3, rx=6)
    p.text(PSU_X + PSU_W / 2, PSU_Y - 18, "RotoPD trigger", 19, INK, "middle", "bold")
    p.rect(PSU_X + 40, PSU_Y + 34, 230, 56, fill="#dfe6ea", stroke=INK, sw=2, rx=4)
    p.text(PSU_X + PSU_W / 2, PSU_Y + 74, volts, 33, INK, "middle", "bold")
    p.text(PSU_X + PSU_W / 2, PSU_Y + 118, "set the PDO before plugging in", 13, MUTE, "middle")
    p.rect(PSU_X - 18, PSU_Y + 34, 18, 20, fill="#d0342c", stroke="none")
    p.mono(PSU_X + 12, PSU_Y + 50, "+", 20, "#d0342c", weight="bold")
    p.rect(PSU_X - 18, PSU_Y + 96, 18, 20, fill="#22262b", stroke="none")
    p.mono(PSU_X + 12, PSU_Y + 112, "-", 20, "#22262b", weight="bold")
    return (PSU_X - 22, PSU_Y + 44), (PSU_X - 22, PSU_Y + 106)


# ===========================================================================
# 3b.  THE BREADBOARD
#
# EVERY NUMBER IN THIS BLOCK IS LAYOUT, NOT A CHECKED FACT.  The guard verifies
# which PIN connects to which PIN, out of pins.h and BENCH_WIRING.md.  It cannot
# verify which HOLE anything sits in, because no source states that - the layout
# is chosen here and the pages say so on their face.  What IS certain and is
# what the rows are derived from: a StepStick's two headers are 0.6 in apart,
# which straddles the centre channel of any 0.1 in breadboard.
#
# The DevKitC-1 deliberately gets no row numbers.  Its header-to-header spacing
# is 0.9 in or 1.0 in depending on the inset, both of which fit and both of
# which land its pins in different columns - and which GPIO sits at which
# position along the header is a board fact this repository does not carry.  So
# the operator reads the silkscreen and writes the row in a blank, which is the
# same "match by LABEL" rule page 3 already applies to the driver.
BB_ROWS = 63                    # full-size 830-point board
BB_COLS_TOP = ["J", "I", "H", "G", "F"]
BB_COLS_BOT = ["E", "D", "C", "B", "A"]

DRIVER_ROW0 = 26                # the EN / VM end of the module
DRIVER_COL_L = "D"              # EN MS1 MS2 PDN CLK STEP DIR (n/c)
DRIVER_COL_R = "H"              # VM GND 2B 2A 1A 1B VIO GND   (0.6 in away)
# THE DEVKITC-1 STAYS OFF THE BREADBOARD, and that is a decision rather than an
# omission.  Its header-to-header spacing is 0.9 in or 1.0 in depending on the
# inset; on an 0.1 in board with an 0.3 in channel the two halves span 1.1 in,
# so BOTH spacings fit and each lands its pins in a different column pair - and
# which GPIO sits where along the header is a board fact this repository does
# not carry.  Guessing bends pins.  So the board sits loose on the bench and
# every wire that reaches it is male-to-FEMALE: male into the breadboard,
# female onto the ESP pin the operator has read off the silkscreen.  That also
# removes the last place a row number could be wrong in a way nobody notices.
JUMPER_COL_L = "B"              # left-side jumpers, two holes clear of the module
JUMPER_COL_R = "J"              # right-side jumpers, at the outer edge
SUPPLY_COL = "I"                # the two RotoPD wires
CAP_COL = "J"                   # the bulk cap, along the outer edge
COIL_COL = "J"                  # the four pigtail leads
# COLUMNS G, F AND E DO NOT EXIST as far as this build is concerned: the module
# body sits over them.  A StepStick is 0.6 in between headers and ~0.8 in wide,
# so the three columns between D and H are under the board and the two outside
# each header are not.  That is why every wire on the right half is in I or J
# and every wire on the left half is in B - not aesthetics, clearance.


def driver_row(pin, which=0):
    """Breadboard row for a driver pin, and the column it sits in."""
    if pin in TMC_LEFT:
        return DRIVER_ROW0 + TMC_LEFT.index(pin), DRIVER_COL_L
    idx = [i for i, n in enumerate(TMC_RIGHT) if n == pin]
    if not idx:
        raise SystemExit("wiringgen: no TMC pin named %r" % pin)
    return DRIVER_ROW0 + idx[which], DRIVER_COL_R


def jumper_hole(pin, which=0):
    """The free hole, on the driver pin's own node, that its wire goes into."""
    row, col = driver_row(pin, which)
    return row, (JUMPER_COL_L if col == DRIVER_COL_L else JUMPER_COL_R)


# Where each connection LANDS, in physical terms.  The PIN names and the ESP
# destinations come from the parsed sources and are checked; the ROW and COLUMN
# are this file's layout choice and every page that prints one says so.
def board_dest(key, drv, dest, which=0):
    row, dcol = driver_row(drv, which)
    jrow, jcol = jumper_hole(drv, which)
    at = "%s is row %d column %s." % (drv, row, dcol)
    if key in ("VIO", "MS1", "MS2"):
        return at + "  Jumper (M-M) row %d column %s  ->  the + rail." % (jrow, jcol)
    if key == "GNDL":
        return at + "  Jumper (M-M) row %d column %s  ->  the - rail." % (jrow, jcol)
    if key == "VM":
        return at + ("  RotoPD  +  on its own 22 AWG wire into row %d column %s."
                     % (row, SUPPLY_COL))
    if key == "GNDP":
        return at + ("  RotoPD  -  on its own 22 AWG wire into row %d column %s."
                     % (row, SUPPLY_COL))
    if key == "PDN":
        return at + "  NOTHING goes in row %d.  Leave the whole row empty." % jrow
    pin = dest.split(":", 1)[1] if dest.startswith("esp:") else dest
    return at + ("  Jumper (M-F) row %d column %s  ->  the ESP pin labelled %s."
                 % (jrow, jcol, pin))


def jumper_counts(table):
    """Derived from the PARSED logic table, so it cannot drift from the pages.

    Three kinds of wire land on the breadboard and they are not interchangeable:
    a row whose destination is a GPIO needs a female end for the loose ESP, a
    row whose destination is a rail is board-internal and needs two male ends,
    and the two rail feeds are the wires that make the rails live at all.
    """
    to_esp = sum(1 for _n, (_d, e) in table.items() if e.startswith("GPIO"))
    to_rail = sum(1 for _n, (_d, e) in table.items() if e in ("3V3", "GND"))
    return {"mf": to_esp + 2, "mm": to_rail, "feeds": 2,
            "total": to_esp + to_rail + 2}


BB_X, BB_Y = 70, 250            # top-left of the drawn board
BB_PITCH = 22                   # px per hole
BB_CHANNEL = 2                  # filler slots = 0.3 in channel, so D..H is 0.6 in


def bb_xy(row, col, y0=BB_Y):
    """Pixel centre of a hole.  Row 1 is at the left."""
    x = BB_X + 46 + (row - 1) * BB_PITCH
    order = BB_COLS_TOP + ["-"] * BB_CHANNEL + BB_COLS_BOT
    y = y0 + 70 + order.index(col) * BB_PITCH
    return x, y


def bb_size(rows=BB_ROWS):
    return (46 + rows * BB_PITCH + 20,
            70 + (len(BB_COLS_TOP) * 2 + BB_CHANNEL) * BB_PITCH + 60)


def draw_breadboard(p, rows=BB_ROWS, label_every=5, hot_rows=(), y0=BB_Y):
    """The board itself: two rail pairs, ten columns, a centre channel."""
    w, h = bb_size(rows)
    p.rect(BB_X, y0, w, h, fill="#fbfbf9", stroke=INK, sw=3, rx=8)

    # power rails, top and bottom
    for i, (yoff, sign, col) in enumerate(((28, "+", "#d0342c"), (48, "-", "#22262b"),
                                           (h - 48, "+", "#d0342c"), (h - 28, "-", "#22262b"))):
        y = y0 + yoff
        p.line(BB_X + 30, y, BB_X + w - 20, y, col, 2, dash="2 6")
        p.text(BB_X + 16, y + 5, sign, 18, col, "middle", "bold")
        p.text(BB_X + w - 10, y + 5, sign, 18, col, "start", "bold")

    # holes
    for c in BB_COLS_TOP + BB_COLS_BOT:
        for r in range(1, rows + 1):
            x, y = bb_xy(r, c, y0)
            on = r in hot_rows
            p.rect(x - 4, y - 4, 8, 8, fill=INK if on else "#d7dce1", stroke="none", rx=1)
    # column letters at both ends
    for c in BB_COLS_TOP + BB_COLS_BOT:
        _x, y = bb_xy(1, c, y0)
        p.mono(BB_X + 30, y + 5, c, 13, MUTE, "end")
    # the channel
    _x0, ytop = bb_xy(1, BB_COLS_TOP[-1], y0)
    _x1, ybot = bb_xy(1, BB_COLS_BOT[0], y0)
    p.rect(BB_X + 38, (ytop + ybot) / 2 - 9, w - 58, 18, fill="#eceff2", stroke="none", rx=3)
    # row numbers
    for r in range(label_every, rows + 1, label_every):
        x, _y = bb_xy(r, "J", y0)
        p.mono(x, y0 + 62, str(r), 12, MUTE, "middle")
    return w, h


def bb_channel_y(y0=BB_Y):
    return (bb_xy(1, BB_COLS_TOP[-1], y0)[1] + bb_xy(1, BB_COLS_BOT[0], y0)[1]) / 2


def wrap_h(s, size, cols, lh):
    """The height p.wrap will consume.  Layout needs it BEFORE the box is drawn."""
    return Page(0, "").wrap(0, 0, s, size, INK, cols=cols, lh=lh)


def draw_bb_module(p, row0, nrows, col_a, col_b, label, fill="#2b3138", y0=BB_Y):
    """A module straddling the channel: row0..row0+nrows-1 between two columns."""
    xa, ya = bb_xy(row0, col_a, y0)
    xb, yb = bb_xy(row0 + nrows - 1, col_b, y0)
    x0, x1 = min(xa, xb) - 9, max(xa, xb) + 9
    y0, y1 = min(ya, yb) - 9, max(ya, yb) + 9
    p.rect(x0, y0, x1 - x0, y1 - y0, fill=fill, stroke=INK, sw=2, rx=4)
    p.text((x0 + x1) / 2, (y0 + y1) / 2 + 6, label, 15, "#ffffff", "middle", "bold")


def run(p, pts, key, label="", dash=None):
    _nm, col, sw = COLOURS[key]
    p.poly(pts, stroke=col, sw=sw, dash=dash)
    p.circle(pts[0][0], pts[0][1], sw * 0.62, col)
    p.circle(pts[-1][0], pts[-1][1], sw * 0.62, col)
    if label:
        mid = pts[len(pts) // 2]
        p.text(mid[0], mid[1] - 14, label, 16, col, "middle", "bold")


# ===========================================================================
# 4.  ENTRY POINT
# ===========================================================================
def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true",
                    help="verify pins.h against BENCH_WIRING.md and exit")
    args = ap.parse_args()

    pins, table = verify(loud=True)
    if args.check:
        return 0

    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
    import wiringgen_pages

    # Running as __main__ means wiringgen_pages imports a SECOND copy of this
    # module, with its own empty FACTS.  Hand the verified facts across.
    wiringgen_pages.FACTS.update(FACTS)

    OUT.mkdir(parents=True, exist_ok=True)
    pages = wiringgen_pages.all_pages(pins, table)
    nums = [pg.num for pg in pages]
    if nums != list(range(1, len(pages) + 1)):
        raise SystemExit("wiringgen: page numbers are not 1..N: %r" % nums)
    for pg in pages:
        # LF regardless of platform, so the committed files are byte-identical
        # whoever renders them - CI diffs them against a fresh run.
        (OUT / ("p%02d.svg" % pg.num)).write_text(pg.svg(len(pages)), encoding="utf-8",
                                                  newline="\n")
    print("wiringgen: wrote %d SVG pages to %s" % (len(pages), OUT))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
