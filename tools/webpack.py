#!/usr/bin/env python3
"""Stage the LittleFS payload: gzipped web assets + the generated ring.json.

Spec 15 phase 3, step (e).  esp_http_server serves `<name>.gz` with
Content-Encoding: gzip when the browser accepts it (components/net/httpd.cpp),
so only the compressed copy ships - a device with 2 MB of filesystem should
not carry both.

    python tools/webpack.py [--out build/webfs] [--budget-kb 256] [--check]

--check prints the report and exits non-zero if the budget is exceeded,
without writing anything.  The build runs it for real; CI runs --check so a
UI that outgrew the partition fails the pipeline rather than the flash.
"""
import argparse
import gzip
import pathlib
import shutil
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Compressed on the way in.  Everything else is copied byte for byte: already
# compressed formats (png, woff2, wav) only get bigger.
COMPRESS = {".html", ".css", ".js", ".json", ".svg", ".txt", ".map"}

# Sources, and where they land in the image.
SOURCES = [
    (ROOT / "web", ""),            # the UI, at the filesystem root
]

# Files the FIRMWARE reads, not the browser.  These must never be compressed:
# ring_store.cpp opens /fs/ring.json directly and knows nothing about gzip, so
# shipping ring.json.gz meant the runtime table never loaded and the compiled
# fallback silently covered for it.  Only a real flash showed it - the boot log
# said "no /fs/ring.json; compiled ring table active" - because the fallback is
# a correct table and nothing downstream looked wrong.
NEVER_COMPRESS = [
    (ROOT / "data" / "ring.json", "ring.json"),  # the runtime ring table (spec 4)
]

# Skipped: the simulator page is a development tool that opens from disk and
# ships nothing to the device.  Its trace file alone is bigger than the UI.
SKIP_DIRS = {"sim"}


def gather():
    """[(source path, name in the image, compress?)], sorted for a stable report."""
    out = []
    for base, prefix in SOURCES:
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if not path.is_file():
                continue
            rel = path.relative_to(base)
            if rel.parts and rel.parts[0] in SKIP_DIRS:
                continue
            name = str(pathlib.PurePosixPath(prefix) / pathlib.PurePosixPath(rel)).lstrip("/")
            out.append((path, name, path.suffix.lower() in COMPRESS))
    for path, name in NEVER_COMPRESS:
        if path.is_file():
            out.append((path, name, False))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(ROOT / "build" / "webfs"))
    ap.add_argument("--budget-kb", type=int, default=256,
                    help="fail if the staged payload exceeds this (partition is 2048 KB)")
    ap.add_argument("--check", action="store_true", help="report only, write nothing")
    args = ap.parse_args()

    items = gather()
    if not items:
        print("webpack: nothing to stage (is web/ missing?)", file=sys.stderr)
        return 1

    out = pathlib.Path(args.out)
    if not args.check:
        if out.exists():
            shutil.rmtree(out)
        out.mkdir(parents=True)

    raw_total = 0
    packed_total = 0
    rows = []
    for path, name, compress in items:
        data = path.read_bytes()
        raw_total += len(data)
        if compress:
            # mtime=0: the image must be byte-identical for identical input, or
            # every build produces a different filesystem.
            blob = gzip.compress(data, compresslevel=9, mtime=0)
            target = name + ".gz"
        else:
            blob = data
            target = name
        packed_total += len(blob)
        rows.append((target, len(data), len(blob)))
        if not args.check:
            dest = out / target
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(blob)

    width = max(len(r[0]) for r in rows)
    print("LittleFS payload:")
    for target, raw, packed in rows:
        pct = (100 * packed // raw) if raw else 100
        print("  {:<{w}}  {:>7} -> {:>7} B  ({:>3}%)".format(target, raw, packed, pct, w=width))
    print("  {:<{w}}  {:>7} -> {:>7} B".format("total", raw_total, packed_total, w=width))
    print("  budget {} KB, partition 2048 KB".format(args.budget_kb))

    if packed_total > args.budget_kb * 1024:
        print("webpack: over budget by {} B".format(packed_total - args.budget_kb * 1024),
              file=sys.stderr)
        return 1
    if not args.check:
        print("webpack: staged in {}".format(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
