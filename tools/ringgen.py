#!/usr/bin/env python3
"""Generate the ring artifacts from the two ring manifests (spec 4).

Inputs (docs/ref/, supplied by the mechanical pipeline):
  manifest_cols1234.json   ring A - columns 1-4, single descending digit block
  manifest_col5.json       ring B - column 5, TWO descending digit blocks

Emits BOTH:
  data/ring.json                                - the runtime table (uploaded
                                                  to LittleFS; the web UI can
                                                  replace it without reflash)
  components/ring/include/ring/ring_table.h     - the compiled-in fallback,
                                                  one table per ring plus the
                                                  per-column assignment

Nobody edits either output by hand.  The manifests are the frozen output of the
flap pipeline and win over every other description of the ring.

Usage:  python tools/ringgen.py [--check]

  --check  exit non-zero if either generated file differs from what is on
           disk, instead of rewriting it.  Used in CI.
"""

import argparse
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
MAN_A = ROOT / "docs" / "ref" / "manifest_cols1234.json"
MAN_B = ROOT / "docs" / "ref" / "manifest_col5.json"
OUT_HEADER = ROOT / "components" / "ring" / "include" / "ring" / "ring_table.h"
OUT_JSON = ROOT / "data" / "ring.json"

N_COLUMNS = 5
# Which manifest drives which column (0-based).  Cols 1-4 share ring A; col 5
# is a distinct part with its own ring.
COLUMN_RING = ["A", "A", "A", "A", "B"]

CATEGORY_ENUM = {
    "blank": "Blank",
    "digit": "Digit",
    "ampm": "AmPm",
    "glyph": "Glyph",
    "wifi": "Wifi",
}

# Colour schemes for the web UI and the simulator, derived from the manifests'
# part_note, which is AUTHORITATIVE here.  Cols 1-3 are the minutes group:
# black card throughout, white inverted digits, red glyphs.  Cols 4-5 are the
# seconds group: white CLOCK cards, and the glyph block is printed on RED stock
# with black ink - "white clock cards, red glyph cards", verbatim.
#
# SETTLED 2026-08-23, do not re-litigate.  This was briefly changed to a white
# card with a red glyph on Nico's instruction and then changed back: he
# confirmed the manifest was right and the instruction was the error.  Both
# directions are now on the record so the next reader does not rediscover the
# question.
#
# The STRADDLE flaps the manifests enumerate separately (straddle_flaps_col4 =
# [1, 37], col 5's straddle_flaps = [0, 14, 24, 39]) are deliberately NOT
# rendered.  They exist to stop colour leak where a dark face would show
# through a light one - a print-side fix, not a visual feature - and a straddle
# flap is half of two adjacent cards, so it has no single card colour to paint
# anyway.
#
# Presentation only.  Firmware behaviour never reads these.
SCHEMES = {
    "minutes": {"card": {"default": "#181818"},
                "ink": {"default": "#e8e4da", "glyph": "#b03a2e"}},
    "seconds": {"card": {"default": "#e8e4da", "glyph": "#b03a2e"},
                "ink": {"default": "#181818"}},
}
COLUMN_SCHEMES = ["minutes", "minutes", "minutes", "seconds", "seconds"]

# The roles each column must be able to satisfy.  Mirrors RingSet::validate_roles
# in components/ring/ring_runtime.cpp - a mismatch there is a build-time bug.
# Column 1 carries AM/PM (spec 7.1); the centre column carries the WiFi glyph.
AMPM_COLUMN = 0
WIFI_COLUMN = N_COLUMNS // 2


def c_string(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')


def die(msg):
    sys.exit("ringgen: " + msg)


def load(path, name):
    with path.open(encoding="utf-8") as f:
        man = json.load(f)
    slots = man["slots"]
    n = man["constants"]["slot_count"]
    if len(slots) != n:
        die(f"{name}: slot_count={n} but {len(slots)} entries")
    for i, s in enumerate(slots):
        if s["index"] != i:
            die(f"{name}: slot {i} has index {s['index']}; must be dense and ordered")
        if s["category"] not in CATEGORY_ENUM:
            die(f"{name}: slot {i} has unknown category {s['category']!r}")
        if not s["char_id"]:
            die(f"{name}: slot {i} has an empty char_id")
    if man["constants"].get("direction") != "descending":
        die(f"{name}: direction is not 'descending' - spec 4 requires it")
    return man


def digit_slots(man):
    """digit -> [slot, ...] in slot order."""
    out = {}
    for s in man["slots"]:
        if s["category"] == "digit":
            out.setdefault(int(s["char_id"]), []).append(s["index"])
    return out


def role_slots(man, category, char_id=None):
    return [s["index"] for s in man["slots"]
            if s["category"] == category and (char_id is None or s["char_id"].lower() == char_id)]


def fwd(a, b, n):
    return (b - a) % n


def check_descending(man, name):
    """Every digit decrement must cost exactly one flip forward - that is what
    'descending' means for a one-way ring, and the countdown depends on it."""
    ds, n = digit_slots(man), man["constants"]["slot_count"]
    for d in range(1, 10):
        if d not in ds or (d - 1) not in ds:
            die(f"{name}: digits are not complete 0-9")
        for src in ds[d]:
            if min(fwd(src, t, n) for t in ds[d - 1]) != 1:
                die(f"{name}: {d}->{d - 1} from slot {src} is not 1 flip - ring is not descending")


def check_roles(man_for_col, name_for_col):
    """Fail generation if a column cannot render something it will be asked for."""
    for col in range(N_COLUMNS):
        man, name = man_for_col[col], name_for_col[col]
        need = {"blank": role_slots(man, "blank"),
                "question": role_slots(man, "glyph", "qmark")}
        for d in range(10):
            need[f"digit {d}"] = digit_slots(man).get(d, [])
        if col == AMPM_COLUMN:
            need["AM"] = role_slots(man, "ampm", "am")
            need["PM"] = role_slots(man, "ampm", "pm")
        if col == WIFI_COLUMN:
            need["wifi"] = role_slots(man, "wifi")
        for role, slots in need.items():
            if not slots:
                die(f"column {col + 1} ({name}) cannot render role '{role}'")


def build_header(mans, meta):
    L = []
    L.append("// GENERATED FILE - DO NOT EDIT.")
    L.append("// Source:     docs/ref/manifest_cols1234.json, docs/ref/manifest_col5.json")
    L.append("// Regenerate: python tools/ringgen.py")
    L.append("//")
    L.append("// Ring version: " + meta["ring_version"] + "  (frozen " + meta["frozen"] + ")")
    L.append("// Direction:    descending - one forward flip DECREMENTS the digit,")
    L.append("//               so a countdown tick is 1 flip and a clock tick is the")
    L.append("//               expensive direction (spec 4, spec 7.1 wear table).")
    L.append("//")
    L.append("// This is the compiled FALLBACK.  The live table is data/ring.json in")
    L.append("// LittleFS; no code may reference a slot index directly - look roles up")
    L.append("// through RingTable/RingSet, which handle column 5's two digit blocks.")
    L.append("#pragma once")
    L.append("")
    L.append('#include "ring/ring_category.h"')
    L.append("")
    L.append("namespace swan {")
    L.append("")
    L.append(f"inline constexpr int RING_SLOT_COUNT   = {mans['A']['constants']['slot_count']};")
    L.append(f"inline constexpr int RING_HOME_SLOT    = {mans['A']['constants']['home_slot']};")
    L.append(f"inline constexpr int RING_COLUMN_COUNT = {N_COLUMNS};")
    L.append("")
    L.append("struct RingSlot {")
    L.append("    const char*  char_id;   // token accepted by the message parser")
    L.append("    const char*  label;     // human-readable, for the UI")
    L.append("    RingCategory category;")
    L.append("};")
    L.append("")

    for key, cname, applies in (("A", "RING_TABLE_A", "columns 1-4"),
                                ("B", "RING_TABLE_B", "column 5")):
        man = mans[key]
        L.append(f"// Ring {key} - {applies}.  {man['applies_to']}")
        L.append(f"inline constexpr RingSlot {cname}[RING_SLOT_COUNT] = {{")
        for s in man["slots"]:
            L.append('    /* {:2d} */ {{ "{}", "{}", RingCategory::{} }},'.format(
                s["index"], c_string(s["char_id"]), c_string(s["label"]),
                CATEGORY_ENUM[s["category"]]))
        L.append("};")
        L.append("")

    L.append("// Which compiled table each column falls back to.")
    L.append("inline constexpr const RingSlot* RING_TABLE_FOR_COLUMN[RING_COLUMN_COUNT] = {")
    L.append("    " + ", ".join("RING_TABLE_" + r for r in COLUMN_RING) + ",")
    L.append("};")
    L.append("")
    L.append("// Presentation only - the drums' colour schemes, so the web UI and the")
    L.append("// simulator mirror what the wall actually looks like even on a board that")
    L.append("// has no ring.json yet.  Firmware behaviour never reads these.")
    L.append("inline constexpr const char* RING_SCHEMES_JSON =")
    L.append('    "{}";'.format(c_string(json.dumps(SCHEMES, separators=(",", ":")))))
    L.append("inline constexpr const char* RING_COLUMN_SCHEME[RING_COLUMN_COUNT] = {")
    L.append("    " + ", ".join('"{}"'.format(s) for s in COLUMN_SCHEMES) + ",")
    L.append("};")
    L.append("")
    L.append("}  // namespace swan")
    L.append("")
    return "\n".join(L)


def slots_doc(man):
    return [{"i": s["index"], "id": s["char_id"], "label": s["label"], "cat": s["category"]}
            for s in man["slots"]]


def build_ring_json(mans, meta):
    columns = []
    for col in range(N_COLUMNS):
        entry = {"scheme": COLUMN_SCHEMES[col]}
        # Columns whose ring differs from the shared table carry their own
        # (spec 4 columns[i].ring).  Cols 1-4 use the shared table.
        if COLUMN_RING[col] != "A":
            entry["ring"] = slots_doc(mans[COLUMN_RING[col]])
        columns.append(entry)

    doc = {
        "schema": 2,
        "generated_from": ["docs/ref/manifest_cols1234.json", "docs/ref/manifest_col5.json"],
        "ring_version": meta["ring_version"],
        "frozen": meta["frozen"],
        "direction": "descending",
        "slot_count": mans["A"]["constants"]["slot_count"],
        "slots": slots_doc(mans["A"]),
        "columns": columns,
        "schemes": SCHEMES,
    }
    return json.dumps(doc, indent=1, ensure_ascii=False) + "\n"


def emit(path, text, check):
    if check:
        current = path.read_text(encoding="utf-8") if path.exists() else None
        if current != text:
            sys.exit(f"{path} is stale - run: python tools/ringgen.py")
        print(f"{path.name} is up to date")
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {path}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    mans = {"A": load(MAN_A, "manifest_cols1234"), "B": load(MAN_B, "manifest_col5")}
    for key, name in (("A", "manifest_cols1234"), ("B", "manifest_col5")):
        check_descending(mans[key], name)

    man_for_col = [mans[COLUMN_RING[c]] for c in range(N_COLUMNS)]
    name_for_col = ["ring " + COLUMN_RING[c] for c in range(N_COLUMNS)]
    check_roles(man_for_col, name_for_col)

    if mans["A"]["ring_version"] != mans["B"]["ring_version"]:
        die("the two manifests are different ring versions")
    meta = {"ring_version": mans["A"]["ring_version"], "frozen": mans["A"]["frozen"]}

    emit(OUT_HEADER, build_header(mans, meta), args.check)
    emit(OUT_JSON, build_ring_json(mans, meta), args.check)


if __name__ == "__main__":
    main()
