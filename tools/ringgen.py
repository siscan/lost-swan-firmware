#!/usr/bin/env python3
"""Generate the ring artifacts from docs/ref/manifest.json (spec 4).

Emits BOTH:
  data/ring.json                                - the runtime table (uploaded
                                                  to LittleFS; the web UI can
                                                  replace it without reflash)
  components/ring/include/ring/ring_table.h     - the compiled-in fallback

Nobody edits either output by hand.  manifest.json is the frozen output of
prodcol.build_ring() and wins over every other description of the ring.

Usage:  python tools/ringgen.py [--check]

  --check  exit non-zero if either generated file differs from what is on
           disk, instead of rewriting it.  Used in CI.
"""

import argparse
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "docs" / "ref" / "manifest.json"
OUT_HEADER = ROOT / "components" / "ring" / "include" / "ring" / "ring_table.h"
OUT_JSON = ROOT / "data" / "ring.json"

CATEGORY_ENUM = {
    "blank": "Blank",
    "digit": "Digit",
    "ampm": "AmPm",
    "glyph": "Glyph",
    "wifi": "Wifi",
}

# Colour schemes for the simulator, from the manifest's column_groups prose:
# cols 1-3 black card / white digits / red glyphs; cols 4-5 white card for
# non-glyph slots, red card for glyphs, black ink throughout.
SCHEMES = {
    "minutes": {"card": {"default": "#181818"},
                "ink": {"default": "#e8e4da", "glyph": "#b03a2e"}},
    "seconds": {"card": {"default": "#e8e4da", "glyph": "#b03a2e"},
                "ink": {"default": "#181818"}},
}
COLUMN_SCHEMES = ["minutes", "minutes", "minutes", "seconds", "seconds"]


def c_string(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')


def validate(manifest):
    consts = manifest["constants"]
    slots = manifest["slots"]
    n = consts["slot_count"]
    if len(slots) != n:
        sys.exit(f"manifest: slot_count={n} but {len(slots)} slot entries")
    for i, s in enumerate(slots):
        if s["index"] != i:
            sys.exit(f"manifest: slot {i} has index {s['index']}; must be dense and ordered")
        if s["category"] not in CATEGORY_ENUM:
            sys.exit(f"manifest: slot {i} has unknown category {s['category']!r}")
    qmark = [s["index"] for s in slots if s["char_id"] == "qmark"]
    if len(qmark) != 1:
        sys.exit(f"manifest: expected exactly one 'qmark' slot, found {qmark}")
    return qmark[0]


def build_header(manifest, qmark):
    consts = manifest["constants"]
    slots = manifest["slots"]
    digit_first, digit_last = consts["digit_block"]
    am_slot, pm_slot = consts["ampm_slots"]

    L = []
    L.append("// GENERATED FILE - DO NOT EDIT.")
    L.append("// Source:     docs/ref/manifest.json")
    L.append("// Regenerate: python tools/ringgen.py")
    L.append("//")
    L.append("// Ring frozen: " + manifest["frozen"])
    L.append("// Integrity:   " + manifest["integrity"]["column5_flap_manifest_crosscheck"])
    L.append("#pragma once")
    L.append("")
    L.append('#include "ring/ring_category.h"')
    L.append("")
    L.append("namespace swan {")
    L.append("")
    L.append(f"inline constexpr int RING_SLOT_COUNT  = {consts['slot_count']};")
    L.append(f"inline constexpr int RING_HOME_SLOT   = {consts['home_slot']};")
    L.append(f"inline constexpr int RING_DIGIT_FIRST = {digit_first};")
    L.append(f"inline constexpr int RING_DIGIT_LAST  = {digit_last};")
    L.append(f"inline constexpr int RING_AM_SLOT     = {am_slot};")
    L.append(f"inline constexpr int RING_PM_SLOT     = {pm_slot};")
    L.append(f"inline constexpr int RING_WIFI_SLOT   = {consts['wifi_slot']};")
    L.append(f"inline constexpr int RING_QMARK_SLOT  = {qmark};")
    L.append("")
    L.append("struct RingSlot {")
    L.append("    const char*  char_id;   // token accepted by the message parser")
    L.append("    const char*  label;     // human-readable, for the UI")
    L.append("    RingCategory category;")
    L.append("};")
    L.append("")
    L.append("inline constexpr RingSlot RING_TABLE[RING_SLOT_COUNT] = {")
    for s in slots:
        L.append('    /* {:2d} */ {{ "{}", "{}", RingCategory::{} }},'.format(
            s["index"], c_string(s["char_id"]), c_string(s["label"]),
            CATEGORY_ENUM[s["category"]]))
    L.append("};")
    L.append("")
    L.append("}  // namespace swan")
    L.append("")
    return "\n".join(L)


def build_ring_json(manifest):
    slots = [{"i": s["index"], "id": s["char_id"], "label": s["label"],
              "cat": s["category"]} for s in manifest["slots"]]
    doc = {
        "schema": 1,
        "generated_from": "docs/ref/manifest.json",
        "frozen": manifest["frozen"],
        "slot_count": manifest["constants"]["slot_count"],
        "slots": slots,
        # Per-column: scheme for the simulator; a column may also carry its own
        # "slots" array here (per-column ring, spec 4) - none do by default.
        "columns": [{"scheme": s} for s in COLUMN_SCHEMES],
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

    with MANIFEST.open(encoding="utf-8") as f:
        manifest = json.load(f)
    qmark = validate(manifest)

    emit(OUT_HEADER, build_header(manifest, qmark), args.check)
    emit(OUT_JSON, build_ring_json(manifest), args.check)


if __name__ == "__main__":
    main()
