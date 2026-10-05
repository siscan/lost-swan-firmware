// The Swan station boot animation (spec §15 phase 7).
//
// Screen-side only: it sends no command, reads no state and never touches the
// flaps.  It owns a full-screen overlay and removes it before the promise
// resolves, so a caller that awaits play() can trust the DOM is clean.
//
// The logo is SUPPLIED ART (web/bootanim_logo.js) - a frame, the eight-trigram
// ring, a disc, a swan and the DHARMA wordmark in one 200x200 system; "THE
// MARK" below says how it is used.  It is vector, so it animates and stays
// sharp from a 195 px phone to a 440 px kiosk; a bitmap would do neither.
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

  // Beats, ms from the first painted frame.  Three ordered draws then the
  // type-in, ~4.4 s all in: this stands between a viewer and a countdown, so it
  // is a flourish on a budget rather than a title sequence.
  const T_FRAME = 0,    S_FRAME = 180;                 // 1. the frame
  const T_RING  = 520,  S_RING  = 105;                 // 2. the trigram ring
  const T_DISC  = 1420;                                // 3. the disc
  // 4. the swan: a pen draws the spines at ONE speed, D_SWAN ms for the whole
  // length of them, with GAP_SWAN between spines, and INK_LAG after the last.
  const T_SWAN  = 1680, D_SWAN  = 1000, GAP_SWAN = 60, INK_LAG = 120;
  const D_MORPH = 420;                                 // ... then spines -> fill
  // 5. the wordmark has no beat of its own: it arrives WITH the ink, at the
  // ink's own speed - see play().  The R and the first A are part of the swan
  // silhouette (compound() says why), so anything earlier would show "DH MA"
  // and then, a moment later, the other two letters.
  const T_TEXT = 3200, D_CHAR = 38;
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

  // --- the swan's spines: a PEN, not a brush ---------------------------------
  // The art supplies each spine as a polyline plus a WIDTH PER VERTEX - the
  // weight of a brush.  Drawn that way, one capsule per segment at that
  // segment's width, the "drawing" was 23 round-capped blobs, 14 of them wider
  // than they were long, all inflating at once: you saw blobs appear, never a
  // line grow (qa.js K-1: "draws as filled blobs, not as inked strokes").
  //
  // So the draw stage is a pen: each spine as ONE path at one thin weight,
  // grown at one speed along its length - the body first, then the neck, which
  // starts where the body stops - and then the existing crossfade inks it into
  // the real silhouette.  The widths stay in the art (and test_logo.js still
  // checks them) but are not used here; the FILL is what carries the swan's
  // weight.
  //
  // One vertex is dropped.  The neck's centreline doubles back on itself around
  // its eighth and ninth vertices - a spur about seven units long, whose far end
  // carries a width of 17.1 on a neck that is about 5 wide.  As a brush stroke
  // that was lost in the weight; as a thin line it is a visible hook.  A vertex
  // where the line turns back more than ~134 degrees is a spur in this sense,
  // and dropping it leaves a smooth path.  This edits the pen's path only,
  // never the silhouette.
  function spineStroke(line) {
    const nums = String(line.d).match(/-?\d+(?:\.\d+)?/g) || [];
    const pts = [];
    for (let i = 0; i + 1 < nums.length; i += 2) {
      pts.push([parseFloat(nums[i]), parseFloat(nums[i + 1])]);
    }
    for (let pass = 0; pass < 6; pass++) {
      let dropped = false;
      for (let i = 1; i + 1 < pts.length; i++) {
        const ax = pts[i][0] - pts[i - 1][0], ay = pts[i][1] - pts[i - 1][1];
        const bx = pts[i + 1][0] - pts[i][0], by = pts[i + 1][1] - pts[i][1];
        const la = Math.hypot(ax, ay), lb = Math.hypot(bx, by);
        if (la > 0 && lb > 0 && (ax * bx + ay * by) / (la * lb) < -0.7) {
          pts.splice(i, 1);
          dropped = true;
          break;
        }
      }
      if (!dropped) break;
    }
    if (pts.length < 2) return null;
    let len = 0;
    let d = "M" + r2(pts[0][0]) + " " + r2(pts[0][1]);
    for (let i = 1; i < pts.length; i++) {
      len += Math.hypot(pts[i][0] - pts[i - 1][0], pts[i][1] - pts[i - 1][1]);
      d += "L" + r2(pts[i][0]) + " " + r2(pts[i][1]);
    }
    return { d: d, len: len };
  }

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

    // The swan: spines first (a pen, drawn), then the fill crossfaded over.
    out.push('<g class="beat-swan">');
    out.push('<g class="spines">');
    for (const line of (L.swan && L.swan.centerlines) || []) {
      const s = spineStroke(line);
      if (s) out.push('<path class="spine" data-len="' + s.len.toFixed(3) + '" d="' + s.d + '"/>');
    }
    out.push('</g>');
    if (L.swan && L.swan.fill) {
      out.push('<path class="swan-fill" fill-rule="evenodd" d="' + C.swan + '"/>');
    }
    out.push('</g>');

    // The wordmark is part of the mark - the Swan patch carries DHARMA across
    // the centre - not a caption under it.  One element per letter, so each
    // counter is punched out of its own letter.
    out.push('<g class="beat-word">');
    for (const d of C.letters) out.push('<path class="part" fill-rule="evenodd" d="' + d + '"/>');
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
    // Hidden by default rather than by script: getTotalLength needs the element
    // in the document, so without this the whole logo flashes complete for one
    // frame before the reveal starts.  1000 is comfortably past the longest
    // path here (the outer octagon, ~563).
    // THE MARK IS FILLED, not stroked - it is artwork, not a diagram - so the
    // parts arrive by revealing rather than by dasharray.  `currentColor` on
    // the <svg> means one colour declaration drives the whole thing.
    s.push("#" + OVERLAY_ID + " svg{color:var(--p-hot,#7CFF9B)}");
    s.push("#" + OVERLAY_ID + " .part{opacity:0;transition:opacity 260ms linear}");
    s.push("#" + OVERLAY_ID + " .part.on{opacity:1}");
    // The letters arrive at the ink's own speed (see play()).  Declared before
    // the `still` rules, which have to win.
    s.push("#" + OVERLAY_ID + " .beat-word .part{transition:opacity " + D_MORPH + "ms linear}");
    // The frame and the ring sit a shade back from the swan, which is the
    // hierarchy the artwork has: the mark is the swan, in a frame.  The mid tone
    // is #43c25e, not the page's --p: terminal.css defines no --p-mid, so the old
    // `var(--p-mid, var(--p, ...))` always resolved to --p (#6ee06e) and the pale
    // swan sat on a disc of nearly its own brightness (1.4:1) - the pen line the
    // swan is drawn with was close to invisible against it.
    s.push("#" + OVERLAY_ID + " .beat-frame .part,#" + OVERLAY_ID +
           " .beat-ring .part{color:var(--p-mid,#43c25e)}");
    s.push("#" + OVERLAY_ID + " .disc{color:var(--p-mid,#43c25e);opacity:0;");
    s.push("transform-box:fill-box;transform-origin:50% 50%;transform:scale(0.82);");
    s.push("transition:opacity 380ms linear,transform 380ms ease-out}");
    s.push("#" + OVERLAY_ID + " .disc.on{opacity:1;transform:scale(1)}");

    // THE SWAN IS ACTUALLY DRAWN: a thin pen line grows along each spine
    // (stroke-dashoffset, linear, so the pen moves at one speed), and then the
    // whole spine group crossfades into the filled silhouette.  Drawn, then
    // inked.  The weight is in the mark's own 200-unit space: ~5 px at the
    // largest size, ~2 px on a phone.
    // Hidden until its own turn.  An armed spine (dash gap = its whole length)
    // still paints a round-capped dot at the pen's starting point, so without
    // this the swan showed a stray dot from the first frame, ahead of the
    // frame, the ring and the disc.  `.on` is added when the spine starts to draw.
    s.push("#" + OVERLAY_ID + " .spine{fill:none;stroke:currentColor;stroke-width:2.6;");
    s.push("stroke-linecap:round;stroke-linejoin:round;opacity:0}");
    s.push("#" + OVERLAY_ID + " .spine.on{opacity:1}");
    s.push("#" + OVERLAY_ID + " .spines{transition:opacity " + D_MORPH + "ms linear}");
    s.push("#" + OVERLAY_ID + ".inked .spines{opacity:0}");
    s.push("#" + OVERLAY_ID + " .swan-fill{opacity:0;transition:opacity " + D_MORPH + "ms linear}");
    s.push("#" + OVERLAY_ID + ".inked .swan-fill{opacity:1}");
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
    // `still` is the reduced-motion and skip state: the finished mark, at once.
    s.push("#" + OVERLAY_ID + ".still .part{opacity:1;transition:none}");
    s.push("#" + OVERLAY_ID + ".still .disc{opacity:1;transform:none;transition:none}");
    s.push("#" + OVERLAY_ID + ".still .spines{opacity:0;transition:none}");
    s.push("#" + OVERLAY_ID + ".still .swan-fill{opacity:1;transition:none}");
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
      if (reduced) root.className = "still inked";
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

      // A FILLED part: it arrives by opacity, on a timer.  The mark is artwork
      // rather than a diagram, so most of it cannot be dash-drawn - only the
      // swan's spines can, and they have their own helper below.
      function reveal(el, delay) {
        if (!el) return;
        timers.push(setTimeout(() => { el.classList.add("on"); }, delay));
      }

      // One spine, dash-drawn at one speed.  `data-len` is the polyline's own
      // length, computed from the data - exact, because these are straight
      // segments - so a browser that will not measure an SVG path still draws it.
      //
      // The dash is rounded UP to a whole unit plus a margin.  A dash shorter
      // than the path, by even a thousandth, leaves the path's far end under the
      // START of the pattern's next dash, which a round cap turns into a full
      // width dot.  The longer dash would reach the end of the path early, so the
      // transition is stretched by the same ratio: the pen arrives at `dur`.
      function armSpine(el, delay, dur) {
        const len = parseFloat(el.getAttribute("data-len")) || 8;
        const pad = Math.ceil(len) + 2;
        el.style.strokeDasharray = pad + " " + pad;
        el.style.strokeDashoffset = String(pad);
        el.style.transition = "stroke-dashoffset " + Math.round(dur * pad / len) + "ms linear " + delay + "ms";
        drawn.push(el);
      }

      function snap() {
        for (let i = 0; i < drawn.length; i++) {
          drawn[i].style.transition = "none";
          drawn[i].style.strokeDasharray = "none";
          drawn[i].style.strokeDashoffset = "0";
        }
        // `still` finishes every filled part and puts the swan straight to ink;
        // one class rather than a walk over a few hundred elements.
        root.classList.add("still", "inked");
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
        // 1. the frame, outer then inner.
        const frame = root.querySelectorAll(".beat-frame .part");
        for (let i = 0; i < frame.length; i++) reveal(frame[i], T_FRAME + i * S_FRAME);

        // 2. the ring, clockwise from the top, and INNER TO OUTER within each
        // trigram - the order the bars are supplied in, and the order a hand
        // draws them.  Staggered per trigram rather than per bar: thirty-six
        // individually timed reveals reads as static, not as a ring arriving.
        const tris = root.querySelectorAll(".tri");
        for (let i = 0; i < tris.length; i++) {
          const bars = tris[i].children;
          for (let j = 0; j < bars.length; j++) {
            reveal(bars[j], T_RING + i * S_RING + j * 26);
          }
        }

        // 3. the disc.
        const disc = root.querySelector(".disc");
        if (disc) reveal(disc, T_DISC);

        // 4. the swan, drawn along its spines and then inked.  One pen speed
        // for all of them (a spine's time is its share of the total length), in
        // the order the art supplies them: the body, then the neck, which starts
        // where the body stops.
        const spines = root.querySelectorAll(".spine");
        let total = 0;
        for (let i = 0; i < spines.length; i++) total += parseFloat(spines[i].getAttribute("data-len")) || 0;
        let at = T_SWAN;
        for (let i = 0; i < spines.length; i++) {
          const len = parseFloat(spines[i].getAttribute("data-len")) || 0;
          const dur = total > 0 ? Math.max(1, Math.round(D_SWAN * len / total)) : D_SWAN;
          armSpine(spines[i], at, dur);
          reveal(spines[i], at);
          at += dur + GAP_SWAN;
        }
        const inkAt = at - GAP_SWAN + INK_LAG;
        after(inkAt, () => { if (root) root.classList.add("inked"); });

        // 5. the wordmark, WITH the ink.  It is part of the mark - the Swan
        // patch carries DHARMA across the centre - so it belongs inside the
        // frame rather than under it as a caption.  The first A and the R are
        // in the swan's silhouette and arrive when it inks, so the other four
        // letters come in at the same moment and the same speed (D_MORPH,
        // set in the stylesheet) and the word resolves as one.
        const word = root.querySelectorAll(".beat-word .part");
        for (let i = 0; i < word.length; i++) reveal(word[i], inkAt + i * 24);

        // Everything is armed; make the browser take the armed values as the
        // starting style, then let the transitions run to their ends.  A forced
        // layout does that.  The two requestAnimationFrame calls this replaced
        // never fire in a hidden tab, so a logo opened in the background never
        // started to draw at all.
        void root.getBoundingClientRect();
        for (let i = 0; i < drawn.length; i++) drawn[i].style.strokeDashoffset = "0";

        after(T_TEXT, () => {
          let i = 0;
          // No key click under the type-in.  It would be the page's first sound
          // and nobody asked for it, and on load there has been no gesture, so
          // the AudioContext is suspended anyway.
          typer = setInterval(() => {
            typed.textContent = TITLE.slice(0, ++i);
            if (i >= TITLE.length) { clearInterval(typer); typer = 0; }
          }, D_CHAR);
        });

        after(T_TEXT + TITLE.length * D_CHAR + 80, () => {
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
