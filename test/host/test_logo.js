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

// ---------------------------------------------------------------------------
// THE MARK AS DRAWN.  Everything above reads the art's DATA; none of it draws
// anything - which is how the mark shipped as a flat green stop sign, the
// trigram ring and the disc invisible on it and the swan "drawn" as blobs
// (qa.js K-1), with every suite green.  This loads the real bootanim_logo.js
// and bootanim.js into a bare context (no DOM is needed to BUILD the markup)
// and reads back what the animation would put on screen.
// ---------------------------------------------------------------------------
const vm = require("vm");
const ctx = vm.createContext({});
vm.runInContext("var window = this;", ctx);
vm.runInContext(src, ctx, { filename: "bootanim_logo.js" });
vm.runInContext(fs.readFileSync(path.join(ROOT, "web", "bootanim.js"), "utf8"), ctx,
                { filename: "bootanim.js" });
const Boot = ctx.window.SwanBoot;
const svg = Boot._logoSvg();
const css = typeof Boot._style === "function" ? Boot._style() : "";

// The runtime ring check the module exposes (its own header says the suite
// asserts on it; until now nothing did).  A second, independent read of the
// same table from the DRAWN bars.
const ringCheck = Boot.checkRing();
eq(ringCheck.ok, true, "SwanBoot.checkRing() agrees with the table" +
   (ringCheck.notes && ringCheck.notes.length ? " - " + ringCheck.notes.join("; ") : ""));

function subpaths(d) {
  return String(d).split(/(?=M)/).map((s) => {
    const n = (s.match(/-?\d+(?:\.\d+)?/g) || []).map(Number);
    const p = [];
    for (let i = 0; i + 1 < n.length; i += 2) p.push([n[i], n[i + 1]]);
    return p;
  }).filter((p) => p.length >= 3);
}
function inside(pt, poly) {
  let c = false;
  for (let i = 0, j = poly.length - 1; i < poly.length; j = i++) {
    const a = poly[i], b = poly[j];
    if ((a[1] > pt[1]) !== (b[1] > pt[1]) &&
        pt[0] < (b[0] - a[0]) * (pt[1] - a[1]) / (b[1] - a[1]) + a[0]) c = !c;
  }
  return c;
}
function drawnPaths(group) {
  const i = svg.indexOf('<g class="' + group + '">');
  const j = svg.indexOf("</g>", i);
  return [...svg.slice(i, j).matchAll(/<path\b([^>]*?)\/>/g)].map((m) => ({
    rule: (m[1].match(/fill-rule="([^"]*)"/) || [])[1] || "",
    d: (m[1].match(/\sd="([^"]*)"/) || [])[1] || "",
  }));
}

// 1. The frame is a RING.  evenodd punches a hole only between subpaths of ONE
// element; two elements is a solid octagon, and the ring and disc - drawn in the
// same colour - vanish into it.
const frameEls = drawnPaths("beat-frame");
eq(frameEls.length, 1, "the frame is ONE element, so evenodd can cut its window");
eq(frameEls.length ? subpaths(frameEls[0].d).length : 0, 2, "that element holds the outer and the inner octagon");
eq(frameEls.length ? frameEls[0].rule : "", "evenodd", "the frame is filled evenodd");

// 2. Every hole shares an element with the shape it punches.  Over the swan and
// the wordmark (one colour, no islands): a subpath inside a subpath of ANOTHER
// element is a counter that would be painted back in.
const swanEls = [...svg.matchAll(/<path class="swan-fill"([^>]*?)\/>/g)].map((m) => ({
  d: (m[1].match(/\sd="([^"]*)"/) || [])[1] || "",
}));
const wordEls = drawnPaths("beat-word");
const hot = [];
swanEls.forEach((e, k) => subpaths(e.d).forEach((p) => hot.push({ el: "swan" + k, p })));
wordEls.forEach((e, k) => subpaths(e.d).forEach((p) => hot.push({ el: "word" + k, p })));
const stray = [];
for (const s of hot) for (const t of hot) {
  if (s !== t && s.el !== t.el && inside(s.p[0], t.p)) stray.push(s.el + " inside " + t.el);
}
eq(stray.join(", "), "", "no counter is drawn as its own element on top of the shape it should punch");

// 3. The shape of the result, which is a property of the delivered art: the R and
// the first A have their OUTLINES in the swan silhouette (the swan's neck crosses
// them), so their counters are punched out of the swan fill.
eq(swanEls.length, 1, "one swan fill element");
eq(swanEls.length ? subpaths(swanEls[0].d).length : 0, 3, "swan fill = the silhouette + the R's and the first A's counters");
eq(wordEls.map((e) => subpaths(e.d).length).join(","), "2,1,1,2", "letters left to right: D(+counter) H M A(+counter)");
eq(wordEls.every((e) => e.rule === "evenodd"), true, "every wordmark element is evenodd");

// 4. THE TRACE.  The mark is drawn the way a vector CRT would draw it: a beam
// goes vertex to vertex along every outline and the line stays lit behind it.
// So EVERY outline in the art must have a traced line and a beam head, built
// from the art's OWN vertices (not a re-drawing of them), in the art's order;
// and while the beam is drawing nothing may fade in as a filled shape ("blobs
// blobbing in", qa.js K-1) - the fills wait until the trace is done.
const rounded = (pts) => pts.map((p) => p.map((v) => Math.round(v * 100) / 100).join(","));
const perimeter = (pts) => {
  let s = 0;
  for (let i = 0; i < pts.length; i++) {
    const a = pts[i], b = pts[(i + 1) % pts.length];
    s += Math.hypot(b[0] - a[0], b[1] - a[1]);
  }
  return s;
};
const outlineEls = (cls) => [...svg.matchAll(new RegExp("<(path|circle) class=\"" + cls + "\" data-len=\"([\\d.]+)\"([^>]*?)/>", "g"))].map((m) => ({
  kind: m[1],
  len: parseFloat(m[2]),
  d: (m[3].match(/\sd="([^"]*)"/) || [])[1] || "",
  r: parseFloat((m[3].match(/\sr="([\d.]+)"/) || [])[1]),
}));
const traces = outlineEls("trace");
const beams = outlineEls("beam");

// every closed polygon in every filled element, in markup order, plus the disc
const ringBars = [];
{
  const i = svg.indexOf('<g class="beat-ring">');
  const j = svg.indexOf('<circle class="part disc"', i);
  for (const m of svg.slice(i, j).matchAll(/<path class="part" d="([^"]*)"\/>/g)) ringBars.push(m[1]);
}
const wantPolys = [];
frameEls.forEach((e) => subpaths(e.d).forEach((p) => wantPolys.push({ g: "frame", v: rounded(p) })));
ringBars.forEach((d) => subpaths(d).forEach((p) => wantPolys.push({ g: "ring", v: rounded(p) })));
swanEls.forEach((e) => subpaths(e.d).forEach((p) => wantPolys.push({ g: "swan", v: rounded(p) })));
wordEls.forEach((e) => subpaths(e.d).forEach((p) => wantPolys.push({ g: "word", v: rounded(p) })));
const discAt = wantPolys.filter((w) => w.g === "frame" || w.g === "ring").length;   // the disc is traced between the ring and the swan

eq(traces.length, wantPolys.length + 1, "one traced line per outline in the art (every filled polygon, and the disc)");
eq(beams.length, traces.length, "and one beam head for each");
eq(/class="spine"|class="spines"/.test(svg), false, "the swan's brush spines are not drawn (the trace is the outline)");

// the vertices the beam visits are the art's own, and in the art's order:
// frame, ring clockwise from the top, disc, swan, wordmark
const polyTraces = traces.filter((t) => t.kind === "path");
let k = 0;
let order = 0;
for (let n = 0; n < traces.length; n++) {
  if (n === discAt) {
    eq(traces[n].kind, "circle", "the disc is traced between the ring and the swan");
    if (traces[n].kind === "circle") {
      eq(Math.abs(traces[n].len - 2 * Math.PI * traces[n].r) < 0.01, true, "the disc's data-len is its circumference");
    }
    continue;
  }
  const want = wantPolys[k++];
  if (!want) break;
  const got = traces[n].kind === "path" ? rounded(subpaths(traces[n].d)[0] || []) : [];
  if (JSON.stringify(got) !== JSON.stringify(want.v)) { order++; fail("outline " + n + " (" + want.g + ") is not drawn from the art's own vertices, in order"); }
}
eq(order, 0, "every outline is drawn from the art's own vertices, in the art's order");

// data-len IS the path's length, or the dash arithmetic cannot land
for (const t of polyTraces) {
  const pts = subpaths(t.d)[0] || [];
  if (Math.abs(perimeter(pts) - t.len) > 0.05) fail("a trace's data-len " + t.len + " is not its perimeter " + perimeter(pts).toFixed(3));
  if (!/Z$/.test(t.d)) fail("a trace is not a closed outline: " + t.d.slice(-20));
}
// the beam is the same path as its line
eq(beams.every((b, n) => b.d === traces[n].d && b.len === traces[n].len), true, "each beam rides the same path as its line");

// nothing fades in as a filled shape while the beam draws
eq(/\.part,#swan-boot \.swan-fill\{opacity:0;/.test(css), true, "every fill is invisible until the trace is done");
eq(/#swan-boot\.settled \.part,#swan-boot\.settled \.swan-fill\{opacity:[\d.]+\}/.test(css), true, "and settles in behind the finished lines");
eq(/\.trace\{[^}]*opacity:0\}/.test(css) && /\.beam\{[^}]*opacity:0\}/.test(css), true, "lines and beams start hidden (an armed round-capped dash is a visible dot)");
eq(/\.trace\.on\{[^}]*opacity:1\}/.test(css) && /\.beam\.on\{[^}]*opacity:1\}/.test(css), true, "and are shown when the beam reaches them");
const tw = css.match(/\.trace\{[^}]*stroke-width:([\d.]+)/);
eq(!!tw && parseFloat(tw[1]) <= 3, true, "the line is thin (stroke-width <= 3 of a 200-unit mark)");
const fa = css.match(/#swan-boot\.settled \.part,#swan-boot\.settled \.swan-fill\{opacity:([\d.]+)\}/);
eq(!!fa && parseFloat(fa[1]) <= 0.6, true, "the fill stays behind the lines (<= 0.6): the lines are the mark, not a filled logo with outlines", fa && fa[1]);

if (failures) {
  console.log(failures + " failure(s)");
  process.exit(1);
}
console.log("all checks passed");
