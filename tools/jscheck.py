"""Crude unterminated-string finder for the web assets.

There is no node on this machine, so a syntax error in app.js only shows up as a
blank page in a browser.  This walks each line as a tiny state machine and
reports any line that ends inside a quote - which is exactly the shape a raw
newline smuggled into a string literal takes.
"""
import sys

FILES = ["web/app.js", "web/terminal.js", "web/flap.js", "web/bus.js"]

bad = 0
for f in FILES:
    src = open(f, encoding="utf-8").read()
    in_block = False
    for i, line in enumerate(src.split("\n"), 1):
        j = 0
        quote = None
        while j < len(line):
            c = line[j]
            if in_block:
                if line.startswith("*/", j):
                    in_block = False
                    j += 2
                    continue
                j += 1
                continue
            if quote is None:
                if line.startswith("//", j):
                    break
                if line.startswith("/*", j):
                    in_block = True
                    j += 2
                    continue
                if c in ("'", '"', "`"):
                    quote = c
                j += 1
                continue
            # inside a string
            if c == chr(92):
                j += 2
                continue
            if c == quote:
                quote = None
            j += 1
        if quote is not None and quote != "`":
            print("%s:%d: line ends inside a %s string: %s" % (f, i, quote, line[:110]))
            bad += 1

print("done, %d suspicious line(s)" % bad)
sys.exit(1 if bad else 0)
