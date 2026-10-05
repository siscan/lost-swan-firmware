// The Swan station boot animation (spec §15 phase 7).
//
// Screen-side only: it sends no command, reads no state and never touches the
// flaps.  It owns a full-screen overlay and removes it before the promise
// resolves, so a caller that awaits play() can trust the DOM is clean.
//
// The logo is SUPPLIED ART (web/bootanim_logo.js) - a frame, the eight-trigram
// ring, a disc, a swan and the DHARMA wordmark in one 200x200 system; "THE
// MARK" below says how it is used.  It is vector, so it stays sharp from a
// 195 px phone to a 440 px kiosk, and it is DRAWN the way a vector CRT would
// draw it: a beam goes vertex to vertex along every outline in the art and the
// line stays lit behind it ("THE TRACE", in the constants).  The art's swan
// centrelines and per-vertex widths describe a brush and are not used.
//
// It plays on every load of terminal.html while the station is SWAN, in both
// content modes, and is always skippable - spec 10.2b says exactly when, in one
// place.  It was gated on a default-off preference until 2026-08-25, which
// meant it had never played for anybody who had not gone looking for the
// toggle first.  Deliberate replays: REPLAY LOGO on the strip, and LOGO at the
// Swan prompt.  The Swan mark is the only one that ships; Pearl and Flame marks
// are optional future art, which is why this is Swan-only rather than
// per-station.
"use strict";

(function (global) {
  const OVERLAY_ID = "swan-boot";
  const TITLE = "STATION 3: THE SWAN";

  // THE TRACE.  A vector CRT draws a shape one way: the beam is deflected to the
  // first vertex, then to the next, and the line it leaves stays lit behind it
  // while a bright spot rides the head.  Every outline in the mark is a closed
  // polygon in the art, so every outline is TRACED - one beam, one speed,
  // blanked between outlines - in the art's own order: the frame, the ring
  // (clockwise from the top, each trigram's bars inner to outer), the disc, the
  // swan, the wordmark.  Nothing fades in as a filled shape while the beam is
  // drawing (qa.js K-1: "blobs blobbing in" is the failure); the fills settle in
  // behind the finished lines at the very end.
  //
  // The beam speed is a consequence, not a constant: TRACE_MS is the budget for
  // the whole mark - this stands between a viewer and a countdown, so it is a
  // flourish on a budget rather than a title sequence - and the speed is
  // whatever fits the mark's total outline into it.
  const T_TRACE = 120;      // the screen is black for a beat, then the beam lights
  const TRACE_MS = 3000;    // the whole trace
  const JUMP_MS = 6;        // the blanked beam repositioning between outlines
  const MIN_SHAPE_MS = 24;  // the shortest outline still takes a visible moment
  const BEAM_LEN = 5;       // the bright head, in the mark's own 200-unit space
  const COOL_MS = 700;      // a just-drawn line falls from hot to normal
  const SETTLE_MS = 420;    // the phosphor fills settle in behind the finished lines
  // ...to this strong.  The LINES are the mark: at 0 it stays pure line art, at
  // 1 it is the filled logo again and the outlines vanish into it.  A quarter
  // keeps the vector look at rest and still reads on a phone, where the lines
  // alone are ~1 px.
  const FILL_A = 0.25;
  const D_CHAR = 38;
  const HOLD_MS = 380;      // the finished logo stands still before it goes
  const FADE_MS = 340;
  const REDUCED_MS = 600;

  // ------------------------------------------------------------------------
  // THE MARK.  Supplied art (web/bootanim_logo.js), adopted wholesale on
  // 2026-08-25.  It replaced a constructed octagon/trigram/wordmark generator
  // that was wrong twice over: the ring was Earlier Heaven where the classic
  // station logos use LATER HEAVEN (King Wen), and it was drawn with the
  // trigrams' bottom lines facing IN when the DHARMA marks face them OUT.
  //
  // ATTRIBUTION, corrected: an earlier note here said the 90-degree variant was
  // Earlier Heaven.  It is not.  Lostpedia attributes the quarter-turn to the
  // ARG-ERA MODERNISED LOGO - "the arrangement of the standard logo was turned
  // 90 degrees clockwise" - and the classic station ring is the un-rotated
  // Later Heaven.  Earlier Heaven does not come into it at all; that was my
  // own construction, and inventing a plausible provenance for a wrong answer
  // is how it survived review.  docs/ref/swan_trigrams.md is the authority.
  //
  // EVERYTHING IS PRE-PROPORTIONED.  The frame, the ring, the disc, the swan
  // and the wordmark share one 200x200 coordinate system and one scale; no
  // part may be scaled independently, and the swan here is not
  // interchangeable with any other.
  //
  // fill-rule EVENODD is required, not decorative: the frame's window and the
  // counters inside the wordmark's letters are holes punched by the rule.
  // Colour is `currentColor` throughout, so the mark takes the phosphor from
  // whatever is above it.
  //
  // The trigram ring is documented and checked: docs/ref/swan_trigrams.md has
  // the authoritative sequence, and ringMatchesTable() below asserts the drawn
  // ring against it at runtime.
  // ------------------------------------------------------------------------
  const CX = 100, CY = 100;

  function logo() {
    return (typeof SWAN_LOGO !== "undefined" && SWAN_LOGO) ||
           (typeof window !== "undefined" && window.SWAN_LOGO) || null;
  }

  const r2 = (v) => Math.round(v * 100) / 100;

  // --- the ring check ------------------------------------------------------
  // Reads the DRAWN bars back out of the data and compares them to
  // docs/ref/swan_trigrams.md.  A solid line is one path, a broken line is two,
  // so clustering the bars by distance from the centre recovers the three lines
  // inner->outer.
  //
  // The four NON-PALINDROMIC trigrams are what this is really for: Li, Kun,
  // Qian and Kan read the same in either direction, so an inside-out ring looks
  // perfectly fine until you check Dui, Gen, Zhen and Xun.  That is exactly the
  // mistake the previous constructed ring made.
  const RING_TABLE = {
    li: "101", kun: "000", dui: "011", qian: "111",
    kan: "010", gen: "100", zhen: "001", xun: "110",
  };
  const RING_ORDER = ["li", "kun", "dui", "qian", "kan", "gen", "zhen", "xun"];
  const RING_TELLS = ["dui", "gen", "zhen", "xun"];

  function ringMatchesTable() {
    const L = logo();
    const out = { ok: true, drawn: {}, order: [], notes: [] };
    if (!L || !L.trigrams) { out.ok = false; out.notes.push("no trigram data"); return out; }
    for (const t of L.trigrams) {
      out.order.push(t.name);
      const dists = t.bars.map((d) => {
        const n = (d.match(/-?\d+(?:\.\d+)?/g) || []).map(Number);
        let sx = 0, sy = 0, c = 0;
        for (let i = 0; i + 1 < n.length; i += 2) { sx += n[i]; sy += n[i + 1]; c++; }
        return Math.hypot(sx / c - CX, sy / c - CY);
      });
      const idx = dists.map((_, i) => i).sort((a, b) => dists[a] - dists[b]);
      const bands = [[idx[0]]];
      for (let i = 1; i < idx.length; i++) {
        if (dists[idx[i]] - dists[idx[i - 1]] > 2.5) bands.push([idx[i]]);
        else bands[bands.length - 1].push(idx[i]);
      }
      const bits = bands.length === 3
          ? bands.map((b) => (b.length === 1 ? "1" : "0")).join("")
          : "?".repeat(bands.length);
      out.drawn[t.name] = bits;
      if (bits !== RING_TABLE[t.name]) {
        out.ok = false;
        out.notes.push(t.name + ": drawn " + bits + ", table " + RING_TABLE[t.name] +
                       (RING_TELLS.indexOf(t.name) >= 0 ? " (a non-palindrome - this is the tell)" : ""));
      }
    }
    if (out.order.join(",") !== RING_ORDER.join(",")) {
      out.ok = false;
      out.notes.push("sequence " + out.order.join(" ") + ", table " + RING_ORDER.join(" "));
    }
    return out;
  }

  // --- compound shapes -------------------------------------------------------
  // fill-rule EVENODD punches a hole only between subpaths of ONE <path>
  // element.  The art lists its outlines and the holes in them as separate
  // entries - the frame is an outer octagon plus an inner one, a letter is an
  // outline plus its counter - so drawing one element per entry fills every
  // hole back in.  That is what shipped on 2026-08-25: the frame was a solid
  // stop sign, the trigram ring and the disc were the same colour as the slab
  // behind them and so were simply not there, and DHARMA's counters were solid.
  // No test could see it, because every test read the DATA and none drew it.
  //
  // So the entries are regrouped: a piece lying inside another piece goes in
  // that piece's element.  Two pieces are counters whose OUTLINES are not in the
  // wordmark at all - the swan's neck crosses the first A and the R, and the art
  // folds those two letters into the swan silhouette - so those two counters are
  // punched out of the swan fill instead.
  //
  // Only plain polylines (M, numbers, Z) can be reasoned about this way.  Art
  // with anything else in it is passed through as supplied rather than guessed
  // at, which is the old behaviour and at least not a new way to be wrong.
  function isPolyline(d) { return /^[MZ0-9\s.,\-]+$/i.test(String(d)); }

  function polygons(d) {
    const out = [];
    const subs = String(d).split(/(?=M)/);
    for (let s = 0; s < subs.length; s++) {
      const nums = subs[s].match(/-?\d+(?:\.\d+)?/g) || [];
      const pts = [];
      for (let i = 0; i + 1 < nums.length; i += 2) pts.push([parseFloat(nums[i]), parseFloat(nums[i + 1])]);
      if (pts.length >= 3) out.push(pts);
    }
    return out;
  }

  function pointInPolygon(p, poly) {
    let inside = false;
    for (let i = 0, j = poly.length - 1; i < poly.length; j = i++) {
      const a = poly[i], b = poly[j];
      if ((a[1] > p[1]) !== (b[1] > p[1]) &&
          p[0] < (b[0] - a[0]) * (p[1] - a[1]) / (b[1] - a[1]) + a[0]) inside = !inside;
    }
    return inside;
  }

  // { frame: [d], swan: d, letters: [d, ...] } - the filled shapes, one entry per
  // element to draw.  Letters run left to right.
  function compound() {
    const L = logo();
    const framePaths = (L && L.frame && L.frame.paths) || [];
    const wordPaths = (L && L.wordmark && L.wordmark.paths) || [];
    const swanFill = (L && L.swan && L.swan.fill) || "";
    if (!framePaths.concat(wordPaths, [swanFill]).every(isPolyline)) {
      return { frame: framePaths, swan: swanFill, letters: wordPaths };
    }

    const swanPolys = polygons(swanFill);
    const pieces = wordPaths.map((d) => ({ d: d, p: polygons(d)[0] })).filter((pc) => pc.p);
    const inSwan = (pc) => swanPolys.some((s) => pointInPolygon(pc.p[0], s));
    const orphans = pieces.filter(inSwan);
    const rest = pieces.filter((pc) => !inSwan(pc));
    const holeOf = (pc) => rest.find((o) => o !== pc && pointInPolygon(pc.p[0], o.p));
    const rootOf = (pc) => {
      let r = pc, o, guard = 8;
      while (guard-- > 0 && (o = holeOf(r))) r = o;
      return r;
    };
    const groups = new Map();
    rest.forEach((pc) => { if (!holeOf(pc)) groups.set(pc, [pc]); });
    rest.forEach((pc) => { if (holeOf(pc)) { const g = groups.get(rootOf(pc)); if (g) g.push(pc); } });
    const minX = (g) => Math.min.apply(null, g[0].p.map((pt) => pt[0]));

    return {
      frame: [framePaths.join(" ")],
      swan: [swanFill].concat(orphans.map((pc) => pc.d)).join(" "),
      letters: Array.from(groups.values())
        .sort((a, b) => minX(a) - minX(b))
        .map((g) => g.map((pc) => pc.d).join(" ")),
    };
  }

  // --- the outlines to trace -------------------------------------------------
  // [{ d, len }] in draw order, one per CLOSED polygon in the art - the vertices
  // the beam visits are the art's own, not a re-drawing of them.  The disc is
  // { circle: [cx, cy, r], len }.  `len` is the perimeter of the points AS
  // WRITTEN into `d` (they are rounded), so it is the path's length rather than
  // an estimate, and the dash arithmetic in play() lands exactly.
  //
  // Art that is not plain polylines is not traced (an empty list) for the reason
  // compound() gives: the fills still settle in, and nothing is guessed at.
  function outlines() {
    const L = logo();
    const out = [];
    if (!L) return out;
    const C = compound();
    const bars = [];
    (L.trigrams || []).forEach((t) => (t.bars || []).forEach((d) => bars.push(d)));
    const sources = ((L.frame && L.frame.paths) || []).concat(
        (L.wordmark && L.wordmark.paths) || [], bars, [(L.swan && L.swan.fill) || ""]);
    if (!sources.every(isPolyline)) return out;

    const push = (pts) => {
      const q = pts.map((p) => [r2(p[0]), r2(p[1])]);
      let len = 0;
      let d = "M" + q[0][0] + " " + q[0][1];
      for (let i = 1; i <= q.length; i++) {
        const a = q[i - 1], b = q[i % q.length];
        len += Math.hypot(b[0] - a[0], b[1] - a[1]);
        if (i < q.length) d += "L" + b[0] + " " + b[1];
      }
      out.push({ d: d + "Z", len: len });
    };
    C.frame.forEach((d) => polygons(d).forEach(push));        // outer octagon, then inner
    bars.forEach((d) => polygons(d).forEach(push));           // clockwise from the top, inner to outer
    if (L.disc) out.push({ circle: [L.disc.cx, L.disc.cy, L.disc.r], len: 2 * Math.PI * L.disc.r });
    polygons(C.swan).forEach(push);                           // the silhouette, then its two counters
    C.letters.forEach((d) => polygons(d).forEach(push));      // left to right, each letter's counter after it
    return out;
  }

  // --- the markup ----------------------------------------------------------
  // Draw order is the art's own: frame, ring, disc, swan, wordmark.
  function svgMarkup() {
    const L = logo();
    if (!L) return "";
    const C = compound();
    const out = [];
    out.push('<svg viewBox="' + (L.viewBox || "0 0 200 200") + '" xmlns="http://www.w3.org/2000/svg"');
    out.push(' fill="currentColor" role="img"');
    out.push(' aria-label="Dharma Initiative Station 3, the Swan">');

    // One element: the frame is a RING (outer octagon minus inner), and only a
    // single path lets evenodd cut the window.
    out.push('<g class="beat-frame">');
    for (const d of C.frame) out.push('<path class="part" fill-rule="evenodd" d="' + d + '"/>');
    out.push('</g>');

    out.push('<g class="beat-ring">');
    for (const t of L.trigrams || []) {
      out.push('<g class="tri" data-name="' + t.name + '">');
      // Inner -> outer, as the data supplies them.
      for (const d of t.bars) out.push('<path class="part" d="' + d + '"/>');
      out.push('</g>');
    }
    out.push('</g>');

    if (L.disc) {
      out.push('<circle class="part disc" cx="' + L.disc.cx + '" cy="' + L.disc.cy +
               '" r="' + L.disc.r + '"/>');
    }

    if (L.swan && L.swan.fill) {
      out.push('<path class="swan-fill" fill-rule="evenodd" d="' + C.swan + '"/>');
    }

    // The wordmark is part of the mark - the Swan patch carries DHARMA across
    // the centre - not a caption under it.  One element per letter, so each
    // counter is punched out of its own letter.
    out.push('<g class="beat-word">');
    for (const d of C.letters) out.push('<path class="part" fill-rule="evenodd" d="' + d + '"/>');
    out.push('</g>');

    // THE TRACE, over the fills: for every outline, the line the beam leaves
    // behind it (.trace) and the bright spot that rides its head (.beam).  The
    // lines come first and the beams after, so a beam is never under a line.
    const T = outlines();
    const shape = (cls, o) => o.circle
        ? '<circle class="' + cls + '" data-len="' + o.len.toFixed(3) + '" cx="' + o.circle[0] +
          '" cy="' + o.circle[1] + '" r="' + o.circle[2] + '" transform="rotate(-90 ' +
          o.circle[0] + ' ' + o.circle[1] + ')"/>'      // so the beam starts at twelve o'clock
        : '<path class="' + cls + '" data-len="' + o.len.toFixed(3) + '" d="' + o.d + '"/>';
    out.push('<g class="traces">');
    for (const o of T) out.push(shape("trace", o));
    out.push('</g><g class="beams">');
    for (const o of T) out.push(shape("beam", o));
    out.push('</g>');

    out.push('</svg>');
    return out.join("");
  }

  // ------------------------------------------------------------------------
  // The overlay's stylesheet, scoped by the overlay's own id and living inside
  // it, so removing the element removes the rules.
  //
  // Colours are the terminal's variables with hex fallbacks: this module may
  // outlive terminal.css (the logo is meant to be reusable), and an unstyled
  // black-on-black logo is indistinguishable from a broken one.
  // ------------------------------------------------------------------------
  function styleTag() {
    const s = [];
    s.push("<style>");
    s.push("#" + OVERLAY_ID + "{position:fixed;inset:0;z-index:60;display:flex;");
    s.push("flex-direction:column;align-items:center;justify-content:center;");
    s.push("gap:clamp(10px,2.6vmin,32px);font-family:inherit;opacity:1;cursor:pointer;");
    s.push("background:radial-gradient(120% 120% at 50% 45%,var(--p-bg,#04120a) 0%,#000 72%);");
    s.push("transition:opacity " + FADE_MS + "ms linear;");
    s.push("-webkit-tap-highlight-color:transparent;user-select:none;-webkit-user-select:none}");
    s.push("#" + OVERLAY_ID + ".fade{opacity:0}");
    // vmin, not px: the same overlay has to read on a 375x812 phone and on a
    // 1080p kiosk, and the logo is the only thing on screen in both.
    s.push("#" + OVERLAY_ID + " svg{width:clamp(168px,52vmin,440px);height:auto;display:block}");
    // `currentColor` on the <svg> means one colour declaration drives the whole
    // mark.  The glow is the phosphor: a tight halo and a wide one, on the <svg>
    // element itself - CSS filter on an SVG child is not portable, on the
    // replaced element it is.
    s.push("#" + OVERLAY_ID + " svg{color:var(--p-hot,#7CFF9B);");
    s.push("filter:drop-shadow(0 0 1.5px rgba(110,224,110,.8)) drop-shadow(0 0 6px rgba(110,224,110,.38))}");

    // THE FILLS WAIT.  While the beam is drawing, nothing may fade in as a filled
    // shape - that is "blobs blobbing in" - so every fill is invisible until the
    // trace is done, and then they settle in behind the finished lines (class
    // `settled`, set by play()).
    s.push("#" + OVERLAY_ID + " .part,#" + OVERLAY_ID + " .swan-fill{opacity:0;");
    s.push("transition:opacity " + SETTLE_MS + "ms linear}");
    s.push("#" + OVERLAY_ID + ".settled .part,#" + OVERLAY_ID + ".settled .swan-fill{opacity:" + FILL_A + "}");
    // The frame, the ring and the disc sit a shade back from the swan, which is
    // the hierarchy the artwork has: the mark is the swan, in a frame.  The mid
    // tone is #43c25e, not the page's --p: terminal.css defines no --p-mid, so a
    // `var(--p-mid, var(--p, ...))` always resolved to --p (#6ee06e), and the
    // pale swan sat on a disc of nearly its own brightness (1.4:1).
    s.push("#" + OVERLAY_ID + " .beat-frame .part,#" + OVERLAY_ID +
           " .beat-ring .part,#" + OVERLAY_ID + " .disc{color:var(--p-mid,#43c25e)}");

    // THE LINES.  A traced outline is hot while the beam is on it and falls to
    // the normal phosphor once the beam has gone (`cool`, added by play()); the
    // weight is in the mark's own 200-unit space - ~3 px at the largest size,
    // ~1.2 px on a phone, which is what a beam does when the screen is smaller.
    // Both start invisible and are shown when the beam reaches them: an armed
    // dash (gap = its whole length) still paints a round-capped dot at the
    // start of its path, and a stray dot per outline, from the first frame, is
    // what this stylesheet used to show.
    s.push("#" + OVERLAY_ID + " .trace{fill:none;stroke:var(--p-hot,#b9ffb9);stroke-width:1.4;");
    s.push("stroke-linecap:round;stroke-linejoin:round;opacity:0}");
    s.push("#" + OVERLAY_ID + " .trace.on{opacity:1}");
    s.push("#" + OVERLAY_ID + " .trace.cool{stroke:var(--p,#6ee06e)}");
    // THE BEAM: a short bright dash riding the head of the line, brighter and
    // wider than anything else on the screen.
    s.push("#" + OVERLAY_ID + " .beam{fill:none;stroke:#f2fff3;stroke-width:3.4;");
    s.push("stroke-linecap:round;opacity:0}");
    s.push("#" + OVERLAY_ID + " .beam.on{opacity:1}");
    // The title is typed over a hidden full-length copy of itself, so the line
    // does not re-centre on every character.
    s.push("#" + OVERLAY_ID + " .cap{position:relative;display:inline-block;");
    s.push("color:var(--p-hot,#7CFF9B);font-size:clamp(11px,2.2vmin,26px);");
    s.push("letter-spacing:0.34em;text-transform:uppercase;text-align:left}");
    s.push("#" + OVERLAY_ID + " .ghost{visibility:hidden}");
    s.push("#" + OVERLAY_ID + " .typed{position:absolute;left:0;top:0;white-space:pre}");
    s.push("#" + OVERLAY_ID + " .prompt{display:flex;align-items:center;gap:0.4em;");
    s.push("opacity:0;color:var(--p-dim,#2f7a3a);font-size:clamp(11px,2vmin,24px);");
    s.push("letter-spacing:0.22em}");
    s.push("#" + OVERLAY_ID + " .prompt.on{opacity:1}");
    s.push("#" + OVERLAY_ID + " .cur{display:inline-block;width:0.55em;height:1.05em;");
    s.push("background:var(--p-hot,#7CFF9B);animation:swanboot-blink 1.06s steps(1,end) infinite}");
    // Namespaced: terminal.css already owns a keyframe called "blink".
    s.push("@keyframes swanboot-blink{0%,49%{opacity:1}50%,100%{opacity:0}}");
    // `still` is the reduced-motion and skip state: the finished mark, at once -
    // every line traced and cooled, every fill settled, no beam.
    s.push("#" + OVERLAY_ID + ".still .part,#" + OVERLAY_ID + ".still .swan-fill{opacity:" + FILL_A + ";");
    s.push("transition:none}");
    s.push("#" + OVERLAY_ID + ".still .trace{opacity:1;stroke:var(--p,#6ee06e);transition:none}");
    s.push("#" + OVERLAY_ID + ".still .beam{opacity:0}");
    s.push("@media (prefers-reduced-motion:reduce){#" + OVERLAY_ID +
           "{transition:none}#" + OVERLAY_ID + " .cur{animation:none}}");
    s.push("</style>");
    return s.join("");
  }

  // ------------------------------------------------------------------------
  let running = null;   // the in-flight promise: a reconnect storm must not
                        // stack overlays on top of each other

  function play(opts) {
    if (running) return running;
    const o = opts || {};
    // THE SECOND HALF OF DEFECT 1 (2026-08-25).  This used to read
    // `if (!prefs || !prefs.boot) return` - the same default-off gate that
    // terminal.js had, in a second file.  Removing one of them changed
    // nothing, which is worth remembering: a feature gated in two places fails
    // exactly as silently after the first fix as before it.
    //
    // WHEN IT PLAYS IS THE CALLER'S DECISION, and it is stated in one place
    // (§10.2b): every load of terminal.html while the station is SWAN, plus
    // the strip's REPLAY LOGO and `LOGO` at the Swan prompt.  Nothing is
    // decided here except that there has to be a document to draw into.
    if (!document.body) return Promise.resolve();

    const skipable = o.skipable !== false;
    // `graceMs`: for this long after it starts, input is CONSUMED but does not
    // skip.  A typed LOGO matches on its fourth letter, and the Enter that
    // naturally follows is a keydown that would land on the skip handler and
    // cut the very animation that was just asked for.
    const grace = Math.max(0, parseInt(o.graceMs, 10) || 0);
    const reduced = !!(global.matchMedia &&
        global.matchMedia("(prefers-reduced-motion: reduce)").matches);

    running = new Promise((resolve) => {
      const startedAt = Date.now();
      const root = document.createElement("div");
      root.id = OVERLAY_ID;
      root.setAttribute("aria-hidden", "true");   // decorative; the page below is the content
      if (reduced) root.className = "still settled";
      root.innerHTML = styleTag() + svgMarkup() +
          '<div class="cap"><span class="ghost">' + TITLE +
          '</span><span class="typed"></span></div>' +
          '<div class="prompt"><span>&gt;:</span><span class="cur"></span></div>';
      document.body.appendChild(root);

      const drawn = [];
      const timers = [];
      let typer = 0;
      let finished = false;

      const after = (ms, fn) => { timers.push(setTimeout(fn, ms)); };
      const typed = root.querySelector(".typed");
      const prompt = root.querySelector(".prompt");

      // Show a line or a beam when the beam reaches it (class `on`).  Until then
      // it is invisible: see the stylesheet for why they must start that way.
      function reveal(el, delay) {
        if (!el) return;
        timers.push(setTimeout(() => { el.classList.add("on"); }, delay));
      }

      // Arm one outline: its line grows along the path and its beam rides the
      // head of it, both at the same speed, both starting `delay` ms from now and
      // taking `dur` ms.  `len` is the path's own length from the data - exact,
      // because the paths are straight segments (and a circle) - so a browser
      // that will not measure an SVG path still draws it correctly.
      //
      // THE LINE is a dash as long as the path, offset by its whole length and
      // slid to zero.  The dash is rounded UP to a whole unit plus a margin: a
      // dash shorter than the path, by even a thousandth, leaves the path's far
      // end under the START of the pattern's next dash, which a round cap turns
      // into a full-width dot.  The longer dash would reach the end of the path
      // early, so the transition is stretched by the same ratio: the head
      // arrives at `dur`.
      //
      // THE BEAM is a short dash with a gap longer than the whole path, so there
      // is exactly one of it, slid from just before the start to just past the
      // end: offset BEAM_LEN puts it in [-BEAM_LEN, 0], and BEAM_LEN - len puts
      // it in [len - BEAM_LEN, len].  Its leading edge is the line's head.
      //
      // Both go into `drawn` as a [line, beam] pair, so play() can set the end
      // states in one pass after a forced layout.
      function armTrace(line, beam, len, delay, dur) {
        const pad = Math.ceil(len) + 2;
        line.style.strokeDasharray = pad + " " + pad;
        line.style.strokeDashoffset = String(pad);
        line.style.transition = "stroke-dashoffset " + Math.round(dur * pad / len) +
            "ms linear " + delay + "ms,stroke " + COOL_MS + "ms linear";
        beam.style.strokeDasharray = BEAM_LEN + " " + (len + 2 * BEAM_LEN + 4);
        beam.style.strokeDashoffset = String(BEAM_LEN);
        beam.style.transition = "stroke-dashoffset " + dur + "ms linear " + delay + "ms";
        drawn.push(line, beam);
      }

      function snap() {
        for (let i = 0; i < drawn.length; i++) {
          drawn[i].style.transition = "none";
          drawn[i].style.strokeDasharray = "none";
          drawn[i].style.strokeDashoffset = "0";
        }
        // `still` finishes everything - every line traced and cooled, every fill
        // settled, the beams gone - with one class rather than a walk over a few
        // hundred elements; `settled` is the state the trace ends in.
        root.classList.add("still", "settled");
        if (typed) typed.textContent = TITLE;
        if (prompt) prompt.classList.add("on");
      }

      function detach() {
        global.removeEventListener("keydown", skip, true);
        global.removeEventListener("pointerdown", skip, true);
        global.removeEventListener("click", skip, true);
      }

      function gone() {
        // Released here and not in end(), so the hold-and-fade beat still eats
        // input: a second keypress from someone mashing a key to skip would
        // otherwise type a digit into the Numbers behind a logo still at full
        // opacity, where nothing on screen shows it landing.
        detach();
        if (root.parentNode) root.parentNode.removeChild(root);
        running = null;
        resolve();
      }

      function end(skipped) {
        if (finished) return;
        finished = true;
        for (let i = 0; i < timers.length; i++) clearTimeout(timers[i]);
        timers.length = 0;
        if (typer) { clearInterval(typer); typer = 0; }
        if (skipped) snap();
        if (reduced) { gone(); return; }
        // The finished logo stands still for a beat, then fades.  Cutting to
        // black on the keypress reads as a crash rather than as a skip.  The
        // promise resolves after the overlay is off the DOM - a caller that
        // awaits it must not have to race the teardown.
        setTimeout(() => {
          root.classList.add("fade");
          setTimeout(gone, FADE_MS);
        }, HOLD_MS);
      }

      function skip(ev) {
        // Consume it.  The window keydown handler in terminal.js would
        // otherwise type the same key into the Numbers, and a tap meant to
        // dismiss the logo would land on whatever is under the overlay.
        if (ev && ev.stopPropagation) ev.stopPropagation();
        if (Date.now() - startedAt < grace) return;   // eaten, not obeyed: see `grace`
        end(true);
      }

      if (reduced) {
        snap();
        after(REDUCED_MS, () => end(false));
      } else {
        // THE TRACE: one beam, one speed, in the art's own order (see the
        // constants).  Each outline's share of the time is its share of the
        // total length, with a floor so the smallest still takes a visible moment.
        const tr = root.querySelectorAll(".trace");
        const bm = root.querySelectorAll(".beam");
        const lens = [];
        let total = 0;
        for (let i = 0; i < tr.length; i++) {
          // Never zero: a [line, beam] pair that is skipped would put every
          // later pair one slot out, and the dash arithmetic divides by this.
          const len = Math.max(parseFloat(tr[i].getAttribute("data-len")) || 0, 0.01);
          lens.push(len);
          total += len;
        }
        // Units per ms: what fits the whole mark into TRACE_MS once the blanked
        // jumps between outlines are paid for.
        const speed = total / Math.max(1, TRACE_MS - tr.length * JUMP_MS);
        let at = T_TRACE;
        for (let i = 0; i < tr.length; i++) {
          const len = lens[i];
          const dur = Math.max(MIN_SHAPE_MS, Math.round(len / speed));
          armTrace(tr[i], bm[i], len, at, dur);
          reveal(tr[i], at);
          reveal(bm[i], at);
          const line = tr[i], beam = bm[i];
          after(at + dur, () => {
            beam.classList.remove("on");             // the beam leaves this outline...
            line.classList.add("cool");              // ...and the line it drew starts to cool
            line.style.strokeDasharray = "none";     // and closes with a proper join where it began
          });
          at += dur + JUMP_MS;
        }

        // The beam has gone; the phosphor fills settle in behind the lines, and
        // the title types as they do.
        const settleAt = at + 40;
        after(settleAt, () => { root.classList.add("settled"); });

        // Everything is armed; make the browser take the armed values as the
        // starting style, then let the transitions run to their ends.  A forced
        // layout does that.  The two requestAnimationFrame calls this replaced
        // never fire in a hidden tab, so a logo opened in the background never
        // started to draw at all.
        void root.getBoundingClientRect();
        for (let i = 0; i + 1 < drawn.length; i += 2) {        // [line, beam] pairs
          drawn[i].style.strokeDashoffset = "0";
          drawn[i + 1].style.strokeDashoffset = String(BEAM_LEN - lens[i / 2]);
        }

        after(settleAt, () => {
          let i = 0;
          // No key click under the type-in.  It would be the page's first sound
          // and nobody asked for it, and on load there has been no gesture, so
          // the AudioContext is suspended anyway.
          typer = setInterval(() => {
            typed.textContent = TITLE.slice(0, ++i);
            if (i >= TITLE.length) { clearInterval(typer); typer = 0; }
          }, D_CHAR);
        });

        after(settleAt + TITLE.length * D_CHAR + 80, () => {
          prompt.classList.add("on");
          end(false);          // end() supplies the hold beat
        });
      }

      if (skipable) {
        global.addEventListener("keydown", skip, true);
        global.addEventListener("pointerdown", skip, true);
        global.addEventListener("click", skip, true);
      }
    });

    return running;
  }

  // TYPE LOGO - THE SWAN'S REPLAY COMMAND, IN THE FRIENDLY TERMINAL.
  //
  // The station screen handles this word itself while it is up (protocol.js);
  // this sniffer is for the other content mode, so that selecting SWAN on the
  // strip means the same thing in both - spec 10.2b: every station's command
  // works in both content modes and in no other station.  It is the same shape
  // as Pearl's LOG and the Flame's CHESS: a word typed at an idle prompt,
  // scoped to its own station, off everywhere else.  Until it existed, LOGO
  // typed in the friendly terminal did nothing at all (qa.js B-6 passed only
  // in protocol mode).
  const LOGO_WORD = "LOGO";
  let logoTyped = "";
  let logoAt = 0;

  if (typeof document !== "undefined" && document.addEventListener) {
    document.addEventListener("keydown", (e) => {
      if (running || e.repeat || e.metaKey || e.ctrlKey || e.altKey) return;
      const t = global.SwanTerm;
      if (!t || !t.station || t.station() !== "swan") return;
      const p = global.SwanProtocol;
      if (p && p.isOn && p.isOn()) return;                 // the station screen owns the keys
      const tag = (e.target && e.target.tagName) || "";
      if (tag === "INPUT" || tag === "TEXTAREA") return;
      if (p && p.idle && !p.idle()) { logoTyped = ""; return; }   // a countdown owns the screen
      if (!e.key || e.key.length !== 1) return;
      const c = e.key.toUpperCase();
      if (c < "A" || c > "Z") { logoTyped = ""; return; }
      const now = Date.now();
      if (now - logoAt > 2000) logoTyped = "";
      logoAt = now;
      const next = logoTyped + c;
      logoTyped = LOGO_WORD.indexOf(next) === 0 ? next : (LOGO_WORD.indexOf(c) === 0 ? c : "");
      if (logoTyped === LOGO_WORD) {
        logoTyped = "";
        play({ skipable: true, graceMs: 700 });
      }
    }, false);
  }

  global.SwanBoot = {
    play,
    _logoSvg: svgMarkup,   // private: the logo without the performance
    _style: styleTag,      // private: the overlay's stylesheet, for the suite
    // The ring, checked against docs/ref/swan_trigrams.md.  Public because the
    // JS suite asserts on it and because a wrong ring is invisible to everyone
    // who has not memorised the bagua: the four palindromic trigrams read the
    // same inside-out, so only Dui, Gen, Zhen and Xun ever show the mistake.
    checkRing: ringMatchesTable,
  };
})(window);
