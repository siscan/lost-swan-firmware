#!/usr/bin/env python3
"""Generate components/ring/include/ring/ring_table.h from docs/ref/manifest.json.

The ring table is data, never typed by hand (CLAUDE.md).  manifest.json is the
frozen output of prodcol.build_ring() and is cross-checked 50/50 against the
printed Column 5 flaps; it wins over every other description of the ring.

Usage:  python tools/gen_ring_table.py [--check]

  --check  exit non-zero if the generated file differs from what is on disk,
           instead of rewriting it.  Use in CI.
"""

import argparse
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "docs" / "ref" / "manifest.json"
OUT = ROOT / "components" / "ring" / "include" / "ring" / "ring_table.h"

CATEGORY_ENUM = {
    "blank": "Blank",
    "digit": "Digit",
    "ampm": "AmPm",
    "glyph": "Glyph",
    "wifi": "Wifi",
}


def c_string(s):
    """Escape a manifest string for a C string literal."""
    return s.replace("\\", "\\\\").replace('"', '\\"')


def build(manifest):
    consts = manifest["constants"]
    slots = manifest["slots"]

    n = consts["slot_count"]
    if len(slots) != n:
        sys.exit(f"manifest: slot_count={n} but {len(slots)} slot entries")
    for i, s in enumerate(slots):
        if s["index"] != i:
            sys.exit(f"manifest: slot {i} has index {s['index']}; array must be dense and ordered")
        if s["category"] not in CATEGORY_ENUM:
            sys.exit(f"manifest: slot {i} has unknown category {s['category']!r}")

    digit_first, digit_last = consts["digit_block"]
    am_slot, pm_slot = consts["ampm_slots"]

    # Derived, not typed: the '?' slot is the ????? full-display state (handoff 2).
    qmark = [s["index"] for s in slots if s["char_id"] == "qmark"]
    if len(qmark) != 1:
        sys.exit(f"manifest: expected exactly one 'qmark' slot, found {qmark}")

    L = []
    L.append("// GENERATED FILE - DO NOT EDIT.")
    L.append("// Source:     docs/ref/manifest.json")
    L.append("// Regenerate: python tools/gen_ring_table.py")
    L.append("//")
    L.append("// Ring frozen: " + manifest["frozen"])
    L.append("// Integrity:   " + manifest["integrity"]["column5_flap_manifest_crosscheck"])
    L.append("#pragma once")
    L.append("")
    L.append('#include "ring/ring_category.h"')
    L.append("")
    L.append("namespace swan {")
    L.append("")
    L.append(f"inline constexpr int RING_SLOT_COUNT  = {n};")
    L.append(f"inline constexpr int RING_HOME_SLOT   = {consts['home_slot']};")
    L.append(f"inline constexpr int RING_DIGIT_FIRST = {digit_first};")
    L.append(f"inline constexpr int RING_DIGIT_LAST  = {digit_last};")
    L.append(f"inline constexpr int RING_AM_SLOT     = {am_slot};")
    L.append(f"inline constexpr int RING_PM_SLOT     = {pm_slot};")
    L.append(f"inline constexpr int RING_WIFI_SLOT   = {consts['wifi_slot']};")
    L.append(f"inline constexpr int RING_QMARK_SLOT  = {qmark[0]};")
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    with MANIFEST.open(encoding="utf-8") as f:
        text = build(json.load(f))

    if args.check:
        current = OUT.read_text(encoding="utf-8") if OUT.exists() else None
        if current != text:
            sys.exit(f"{OUT} is stale - run: python tools/gen_ring_table.py")
        print(f"{OUT.name} is up to date")
        return

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {OUT} ({text.count(chr(10))} lines)")


if __name__ == "__main__":
    main()
