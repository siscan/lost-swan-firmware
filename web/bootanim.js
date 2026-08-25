// The Swan station boot animation (spec §15 phase 7).
//
// Screen-side only: it sends no command, reads no state and never touches the
// flaps.  It owns a full-screen overlay and removes it before the promise
// resolves, so a caller that awaits play() can trust the DOM is clean.
//
// The logo is an ORIGINAL vector built here from geometry - two concentric
// regular octagons, the eight I Ching trigrams one per edge, a stylised swan -
// for the same reason web/glyphs.svg is original artwork rather than a traced
// screengrab.  Paths also mean it stroke-animates and stays sharp from a 195 px
// phone to a 440 px kiosk; a bitmap would do neither.
//
// Off by default like every Phase 7 flourish (SwanTerm.prefs.boot).  Nothing on
// this page may appear because a module was loaded.
"use strict";

(function (global) {
  const OVERLAY_ID = "swan-boot";
  const TITLE = "STATION 3: THE SWAN";

  // Beats, ms from the first painted frame.  Three ordered draws then the
  // type-in, ~4.4 s all in: this stands between a viewer and a countdown, so it
  // is a flourish on a budget rather than a title sequence.
  const T_FRAME = 0,    D_FRAME = 520, S_FRAME = 180;  // 1. the frame
  const T_RING  = 520,  D_RING  = 260, S_RING  = 105;  // 2. the trigram ring
  const T_DISC  = 1420, D_DISC  = 380;                 // 3. the disc
  const T_SWAN  = 1680, D_SWAN  = 620, S_SWAN = 26;    // 4. the swan's spines
  const D_MORPH = 420;                                 // ... then spines -> fill
  const T_WORD  = 2760, D_WORD  = 420;                 // 5. the wordmark
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

  // --- the swan's variable-width spines ------------------------------------
  // The art supplies each spine as a polyline plus a WIDTH PER VERTEX, because
  // a swan drawn with a brush is not one weight throughout.  SVG cannot vary a
  // stroke along a path, so each segment becomes its own short path carrying
  // the mean of its two endpoint widths.  Round caps and joins make the joins
  // between segments invisible, which is the whole trick.
  function spineSegments(line) {
    const nums = String(line.d).match(/-?\d+(?:\.\d+)?/g) || [];
    const pts = [];
    for (let i = 0; i + 1 < nums.length; i += 2) {
      pts.push([parseFloat(nums[i]), parseFloat(nums[i + 1])]);
    }
    const w = line.widths || [];
    const segs = [];
    for (let i = 0; i + 1 < pts.length; i++) {
      const a = pts[i], b = pts[i + 1];
      const wa = typeof w[i] === "number" ? w[i] : 6;
      const wb = typeof w[i + 1] === "number" ? w[i + 1] : wa;
      segs.push({
        d: "M" + r2(a[0]) + " " + r2(a[1]) + "L" + r2(b[0]) + " " + r2(b[1]),
        width: r2((wa + wb) / 2),
        len: Math.hypot(b[0] - a[0], b[1] - a[1]),
      });
    }
    return segs;
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

  // --- the markup ----------------------------------------------------------
  // Draw order is the art's own: frame, ring, disc, swan, wordmark.
  function svgMarkup() {
    const L = logo();
    if (!L) return "";
    const out = [];
    out.push('<svg viewBox="' + (L.viewBox || "0 0 200 200") + '" xmlns="http://www.w3.org/2000/svg"');
    out.push(' fill="currentColor" role="img"');
    out.push(' aria-label="Dharma Initiative Station 3, the Swan">');

    out.push('<g class="beat-frame">');
    for (const d of (L.frame && L.frame.paths) || []) {
      out.push('<path class="part" fill-rule="evenodd" d="' + d + '"/>');
    }
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

    // The swan: spines first (stroked, drawn), then the fill crossfaded over.
    out.push('<g class="beat-swan">');
    out.push('<g class="spines">');
    for (const line of (L.swan && L.swan.centerlines) || []) {
      for (const seg of spineSegments(line)) {
        out.push('<path class="spine" data-len="' + r2(seg.len) +
                 '" style="stroke-width:' + seg.width + '" d="' + seg.d + '"/>');
      }
    }
    out.push('</g>');
    if (L.swan && L.swan.fill) {
      out.push('<path class="swan-fill" fill-rule="evenodd" d="' + L.swan.fill + '"/>');
    }
    out.push('</g>');

    // The wordmark is part of the mark - the Swan patch carries DHARMA across
    // the centre - not a caption under it.
    out.push('<g class="beat-word">');
    for (const d of (L.wordmark && L.wordmark.paths) || []) {
      out.push('<path class="part" fill-rule="evenodd" d="' + d + '"/>');
    }
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
    // The frame and the ring sit a shade back from the swan, which is the
    // hierarchy the artwork has: the mark is the swan, in a frame.
    s.push("#" + OVERLAY_ID + " .beat-frame .part,#" + OVERLAY_ID +
           " .beat-ring .part{color:var(--p-mid,var(--p,#43c25e))}");
    s.push("#" + OVERLAY_ID + " .disc{color:var(--p-mid,var(--p,#43c25e));opacity:0;");
    s.push("transform-box:fill-box;transform-origin:50% 50%;transform:scale(0.82);");
    s.push("transition:opacity 380ms linear,transform 380ms ease-out}");
    s.push("#" + OVERLAY_ID + " .disc.on{opacity:1;transform:scale(1)}");

    // THE SWAN IS ACTUALLY DRAWN.  The art carries centreline spines with a
    // width per vertex precisely so it can be: each segment is stroked with a
    // dasharray, round-capped so the joins vanish, and then the whole spine
    // group crossfades into the filled silhouette.  Drawn, then inked.
    s.push("#" + OVERLAY_ID + " .spine{fill:none;stroke:currentColor;");
    s.push("stroke-linecap:round;stroke-linejoin:round;opacity:0.92}");
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
    // Default off, and absent means off: nothing here may force itself on.
    const prefs = global.SwanTerm && global.SwanTerm.prefs;
    if (!prefs || !prefs.boot || !document.body) return Promise.resolve();

    const skipable = o.skipable !== false;
    const reduced = !!(global.matchMedia &&
        global.matchMedia("(prefers-reduced-motion: reduce)").matches);

    running = new Promise((resolve) => {
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

      // One spine segment, dash-drawn.  `data-len` is measured from the data
      // rather than from getTotalLength, so a browser that will not measure an
      // SVG path still draws it correctly - and these are straight segments, so
      // the arithmetic is exact rather than an approximation.
      function armSpine(el, delay, dur) {
        const len = parseFloat(el.getAttribute("data-len")) || 8;
        el.style.strokeDasharray = len + " " + len;
        el.style.strokeDashoffset = String(len);
        el.style.transition = "stroke-dashoffset " + dur + "ms ease-out " + delay + "ms";
        drawn.push(el);
      }

      function arm(el, delay, dur) {
        let len = 0;
        try { len = el.getTotalLength ? el.getTotalLength() : 0; } catch (_) { len = 0; }
        // A browser that will not measure still gets a reveal rather than a
        // path stuck permanently in the dash gap.
        if (!len) len = 400;
        el.style.strokeDasharray = len + " " + len;
        el.style.strokeDashoffset = String(len);
        el.style.transition = "stroke-dashoffset " + dur + "ms ease-out " + delay + "ms";
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

        // 4. the swan, drawn along its spines and then inked.
        const spines = root.querySelectorAll(".spine");
        for (let i = 0; i < spines.length; i++) armSpine(spines[i], T_SWAN + i * S_SWAN, D_SWAN);
        const inkAt = T_SWAN + spines.length * S_SWAN + D_SWAN;
        after(inkAt, () => { if (root) root.classList.add("inked"); });

        // 5. the wordmark, last, as the ink settles.  It is part of the mark -
        // the Swan patch carries DHARMA across the centre - so it belongs
        // inside the frame rather than under it as a caption.
        const word = root.querySelectorAll(".beat-word .part");
        for (let i = 0; i < word.length; i++) reveal(word[i], T_WORD + i * 24);

        // Two frames: one for the browser to take the armed values as the
        // starting style, one to transition away from them.
        requestAnimationFrame(() => requestAnimationFrame(() => {
          if (finished) return;
          for (let i = 0; i < drawn.length; i++) drawn[i].style.strokeDashoffset = "0";
        }));

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

  global.SwanBoot = {
    play,
    _logoSvg: svgMarkup,   // private: the logo without the performance
    // The ring, checked against docs/ref/swan_trigrams.md.  Public because the
    // JS suite asserts on it and because a wrong ring is invisible to everyone
    // who has not memorised the bagua: the four palindromic trigrams read the
    // same inside-out, so only Dui, Gen, Zhen and Xun ever show the mistake.
    checkRing: ringMatchesTable,
  };
})(window);
