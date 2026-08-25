// The Swan mark's trigram ring, against docs/ref/swan_trigrams.md.
//
// WHY THIS IS A TEST AND NOT AN EYEBALL. The classic DHARMA station logos use
// the Later Heaven (King Wen) bagua with every trigram INVERTED
// inside-to-outside: the bottom line faces outward, so the trigram's top line
// is the innermost bar. Four of the eight - Li, Kun, Qian and Kan - are
// palindromes and read identically either way, so a ring built inside-out looks
// completely convincing until you check the other four.
//
// This project has already shipped a wrong ring once, derived from bagua theory
// instead of from reference, and nobody spotted it by looking. Dui, Gen, Zhen
// and Xun are the tells; they are what this asserts.
"use strict";

const fs = require("fs");
const path = require("path");

const ROOT = path.join(__dirname, "..", "..");
const LOGO = path.join(ROOT, "web", "bootanim_logo.js");
const DOC = path.join(ROOT, "docs", "ref", "swan_trigrams.md");

let failures = 0;
function fail(what) {
  console.log("FAIL " + what);
  ++failures;
}
function eq(got, want, what) {
  if (got !== want) fail(what + "  (" + got + " vs " + want + ")");
}

// ---------------------------------------------------------------------------
// The table, parsed OUT OF THE DOCUMENT rather than copied into this file, so
// the two cannot drift apart silently. The doc is the authority; if somebody
// edits it, this test follows.
// ---------------------------------------------------------------------------
const doc = fs.readFileSync(DOC, "utf8");
const table = [];
for (const line of doc.split("\n")) {
  // | 3 (right) | Dui ☱ | lake | 1 1 0 | **0 1 1** |
  const m = line.match(/^\|\s*\d+[^|]*\|\s*([A-Z][a-z]+)[^|]*\|[^|]*\|([^|]*)\|([^|]*)\|/);
  if (!m) continue;
  const name = m[1].toLowerCase();
  const canonical = m[2].replace(/[^01]/g, "");
  const render = m[3].replace(/[^01]/g, "");
  if (canonical.length === 3 && render.length === 3) table.push({ name, canonical, render });
}
eq(table.length, 8, "parsed eight trigrams out of swan_trigrams.md");

// The document's own two columns must be consistent: `render` is `canonical`
// reversed, because that IS what "inverted inside-to-outside" means. If this
// fails, the documentation is wrong and the art may be fine.
for (const t of table) {
  const reversed = t.canonical.split("").reverse().join("");
  eq(t.render, reversed, t.name + ": render is canonical reversed");
}

// And the four palindromes really are the four named as palindromes.
const palindromes = table.filter((t) => t.canonical === t.render).map((t) => t.name).sort();
eq(palindromes.join(","), "kan,kun,li,qian", "the palindromic four");

// ---------------------------------------------------------------------------
// The art, read back out of the delivered data.
// ---------------------------------------------------------------------------
const src = fs.readFileSync(LOGO, "utf8");
if (/np\.float64/.test(src)) {
  fail("bootanim_logo.js still contains np.float64(...) - it parses but throws " +
       "ReferenceError on evaluation, which blanks the page");
}

const blocks = [...src.matchAll(/\{\s*name:"(\w+)",\s*bars:\[([\s\S]*?)\]\s*\}/g)];
eq(blocks.length, 8, "eight trigram groups in the art");

const CX = 100, CY = 100;

function bars(body) {
  return [...body.matchAll(/"([^"]+)"/g)].map((m) => m[1]);
}

function centroidDistance(d) {
  const n = (d.match(/-?\d+(?:\.\d+)?/g) || []).map(Number);
  let sx = 0, sy = 0, c = 0;
  for (let i = 0; i + 1 < n.length; i += 2) { sx += n[i]; sy += n[i + 1]; c++; }
  return Math.hypot(sx / c - CX, sy / c - CY);
}

// The centroid of every point in every bar of one trigram.
//
// The angle is taken from the COMBINED centroid, not by averaging each bar's
// angle: the top trigram straddles 0 degrees, so its bars sit at about 350 and
// about 10, and a plain mean of those is 180 - the opposite side of the ring.
// That circular-mean trap put Li at "91.9 degrees" on the first run of this
// test, which is a bug in the test and not in the art.
function ringAngle(paths) {
  let sx = 0, sy = 0, c = 0;
  for (const d of paths) {
    const n = (d.match(/-?\d+(?:\.\d+)?/g) || []).map(Number);
    for (let i = 0; i + 1 < n.length; i += 2) { sx += n[i]; sy += n[i + 1]; c++; }
  }
  return (Math.atan2(sx / c - CX, CY - sy / c) * 180 / Math.PI + 360) % 360;
}

const drawnOrder = [];
for (let i = 0; i < blocks.length; i++) {
  const name = blocks[i][1];
  const paths = bars(blocks[i][2]);
  drawnOrder.push(name);

  // A solid line is one path; a broken line is two. Cluster the bars by
  // distance from the centre and each cluster is one line, inner to outer.
  const dists = paths.map(centroidDistance);
  const order = dists.map((_, k) => k).sort((a, b) => dists[a] - dists[b]);
  const bands = [[order[0]]];
  for (let k = 1; k < order.length; k++) {
    if (dists[order[k]] - dists[order[k - 1]] > 2.5) bands.push([order[k]]);
    else bands[bands.length - 1].push(order[k]);
  }

  const row = table.find((t) => t.name === name);
  if (!row) { fail("art has a trigram the table does not: " + name); continue; }

  if (bands.length !== 3) {
    fail(name + ": could not resolve three lines (got " + bands.length + ")");
    continue;
  }
  const bits = bands.map((b) => (b.length === 1 ? "1" : "0")).join("");
  const tell = ["dui", "gen", "zhen", "xun"].indexOf(name) >= 0;
  eq(bits, row.render, name + ": drawn inner->outer" + (tell ? "  [NON-PALINDROME - the tell]" : ""));

  // Position: 45 degrees apart, clockwise from the top.
  const a = ringAngle(paths);
  const want = i * 45;
  const off = Math.abs(((a - want + 180) % 360) - 180);
  if (off > 12) fail(name + ": sits at " + a.toFixed(1) + " deg, expected ~" + want);
}

eq(drawnOrder.join(" "), table.map((t) => t.name).join(" "),
   "sequence clockwise from top matches the table");

// ---------------------------------------------------------------------------
// The rest of the mark's structure, which the animation depends on.
// ---------------------------------------------------------------------------
eq(/viewBox:\s*"0 0 200 200"/.test(src), true, "viewBox is 0 0 200 200");
eq(/fill(?:-|R)ule/i.test(src), true, "fill-rule is declared (evenodd is load-bearing)");
eq((src.match(/fillRule:\s*"evenodd"/g) || []).length >= 2, true,
   "frame and wordmark both declare evenodd");
eq(/disc:\s*\{\s*"?cx"?\s*:/.test(src), true, "the disc is supplied as cx/cy/r");
eq(/centerlines:/.test(src), true, "the swan supplies centreline spines");
eq(/widths:\s*\[/.test(src), true, "the spines supply a width per vertex");

// Every spine's width count must match its vertex count, or the interpolation
// silently falls back to a default and the swan draws at the wrong weight.
for (const m of src.matchAll(/d:"([^"]+)",\s*widths:\[([^\]]*)\]/g)) {
  const verts = ((m[1].match(/-?\d+(?:\.\d+)?/g) || []).length) / 2;
  const widths = m[2].split(",").filter((x) => x.trim().length).length;
  eq(widths, verts, "spine has one width per vertex");
}

if (failures) {
  console.log(failures + " failure(s)");
  process.exit(1);
}
console.log("all checks passed");
