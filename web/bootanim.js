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
  const T_OCT = 0,    D_OCT = 880,  S_OCT = 140;   // 1. the octagon
  const T_RING = 780, D_RING = 300, S_RING = 105;  // 2. the trigram ring
  const T_SWAN = 1760, D_SWAN = 620, S_SWAN = 80;  // 3. the swan
  const T_TEXT = 2800, D_CHAR = 38;
  const HOLD_MS = 380;      // the finished logo stands still before it goes
  const FADE_MS = 340;
  const REDUCED_MS = 600;

  // ------------------------------------------------------------------------
  // Geometry, in the 200x200 viewBox.
  //
  // Vertices sit at 22.5 + k*45 degrees so the octagon's flats are top, bottom,
  // left and right - the orientation the logo is always drawn in.  The band
  // between the two octagons is 24 units wide and carries one trigram per edge.
  // ------------------------------------------------------------------------
  const CX = 100, CY = 100;
  const R_OUT = 92, R_IN = 66;
  const BAR_R = [65.5, 73, 80.5];   // the three bars, innermost first
  const BAR_HALF = 14;              // half a bar's length, along the edge
  const GAP_HALF = 4.6;             // half the break in a yin bar

  // THE BAGUA, in the EARLIER HEAVEN (Fu Xi) arrangement, clockwise from the
  // top.  Written bottom-line-first, because on a bagua ring a trigram's bottom
  // line is the one facing the centre - so index 0 is the INNERMOST bar, which
  // is the order BAR_R is in.  1 = solid (yang), 0 = broken (yin).
  //
  //   k=0  top          Qian  heaven    111   three solid
  //   k=1               Dui   lake      110
  //   k=2               Li    fire      101
  //   k=3               Zhen  thunder   100
  //   k=4  bottom       Kun   earth     000   three broken
  //   k=5               Gen   mountain  001
  //   k=6               Kan   water     010
  //   k=7               Xun   wind      011
  //
  // Two properties identify this as a real arrangement rather than eight
  // decorative dashes, and both are worth checking if anyone edits the array:
  // every one of the eight three-bit patterns appears EXACTLY ONCE, and each
  // trigram sits diametrically opposite its exact complement (Qian/Kun,
  // Dui/Gen, Li/Kan, Zhen/Xun).  The second is what Earlier Heaven means.
  const TRIGRAMS = ["111", "110", "101", "100", "000", "001", "010", "011"];

  // ======================================================================
  // PLACEHOLDER ART - THE SWAN SILHOUETTE.  REPLACE THIS BLOCK.
  // ======================================================================
  //
  // Everything else in this file is constructed geometry: the octagons are
  // computed from a radius and an angle, and the trigrams are one authored
  // group rotated eight times.  The swan is not, and cannot honestly be - the
  // real mark's silhouette is a specific drawing, and constructing one from a
  // description produces something that reads as a duck.  Rather than pass that
  // off as the logo it is fenced off here and labelled.
  //
  // TO REPLACE: drop in the vector art the same way the flap glyphs arrived
  // (spec 17, 2026-08-23 - an original vector, not a traced screengrab), as
  // paths in this same 200x200 viewBox with the mark centred on (100, 100).
  // Nothing outside these two constants needs to change: the draw-on animation
  // walks the array in order, so authoring the paths in the order a hand draws
  // them is all the staging it needs.
  //
  // What the pose should be, for whoever draws it: a long recurved neck, the
  // head carried low and forward over the back, and a body made of sweeping
  // strokes rather than an outline.
  //
  // Until then this stands in - deliberately simple, and deliberately not
  // pretending to be the mark.
  const SWAN_IS_PLACEHOLDER = true;
  const SWAN = [
    // Body: one sweeping stroke, tail high at the left, breast low at the right.
    ["swan", "M62 128C66 108 84 96 106 96C124 96 138 104 144 116C136 124 120 130 " +
             "102 131C86 132 70 131 62 128Z"],
    // Neck: the recurve - up from the breast, back over the body, head low.
    ["swan", "M120 99C122 86 118 74 108 68C100 63 90 65 87 72C85 78 89 84 96 84"],
    // Head and beak, carried low and forward.
    ["swan", "M96 84C90 84 85 81 84 77C83 73 86 70 90 70L78 66"],
    // Water.
    ["swan wake", "M58 137H104"],
    ["swan wake", "M114 140H146"],
  ];

  // The DHARMA wordmark, centred under the mark.  It is set as text rather than
  // outlined because the page's own terminal face is already loaded and an
  // outlined wordmark would be several kB of path for six letters.
  const WORDMARK = "DHARMA";

  const r2 = (v) => Math.round(v * 100) / 100;

  function octagonPath(r) {
    const pts = [];
    for (let k = 0; k < 8; k++) {
      const a = (22.5 + k * 45) * Math.PI / 180;
      pts.push(r2(CX + r * Math.cos(a)) + " " + r2(CY + r * Math.sin(a)));
    }
    return "M" + pts.join("L") + "Z";
  }

  function bar(x, y1, y2) {
    return '<line class="bar" x1="' + x + '" y1="' + r2(y1) +
           '" x2="' + x + '" y2="' + r2(y2) + '"/>';
  }

  // One trigram, authored at 0 degrees (bars vertical, on the right-hand edge)
  // and rotated into place.  Rotating one authored group is the only way this
  // stays auditable - eight hand-computed copies would drift.
  function trigram(bits, deg) {
    let s = '<g class="tri" transform="rotate(' + deg + ' ' + CX + ' ' + CY + ')">';
    for (let i = 0; i < 3; i++) {
      const x = r2(CX + BAR_R[i]);
      if (bits.charAt(i) === "1") {
        s += bar(x, CY - BAR_HALF, CY + BAR_HALF);
      } else {
        s += bar(x, CY - BAR_HALF, CY - GAP_HALF);
        s += bar(x, CY + GAP_HALF, CY + BAR_HALF);
      }
    }
    return s + "</g>";
  }

  // The finished markup, with no animation state on it.  Exposed privately so
  // the logo can be reused - a header mark, the Pearl printout - without
  // replaying anything.
  function svgMarkup() {
    const out = [];
    out.push('<svg viewBox="0 0 200 200" xmlns="http://www.w3.org/2000/svg"');
    out.push(' role="img" aria-label="Dharma Initiative Station 3, the Swan">');
    out.push('<g class="beat-oct">');
    out.push('<path class="ring" d="' + octagonPath(R_OUT) + '"/>');
    out.push('<path class="ring" d="' + octagonPath(R_IN) + '"/>');
    out.push('</g><g class="beat-ring">');
    // k = 0 is the top edge; rotate() measures from +x, and +y is down.
    for (let k = 0; k < 8; k++) out.push(trigram(TRIGRAMS[k], k * 45 - 90));
    out.push('</g>');
    // Scaled about the centre so the beak and the tail clear the inner octagon
    // with room to spare; the paths above are authored at full size.
    out.push('<g class="swan-g" transform="translate(100 100) scale(0.82) translate(-100 -100)">');
    for (let i = 0; i < SWAN.length; i++) {
      out.push('<path class="' + SWAN[i][0] + '" d="' + SWAN[i][1] + '"/>');
    }
    out.push('</g>');
    // THE WORDMARK, part of the mark rather than a caption: it sits inside the
    // octagon, centred, below the swan.  It draws with the swan beat because
    // text cannot be stroke-revealed the way a path can - a dasharray on a
    // <text> would animate its outline, which is not the same gesture.
    out.push('<text class="wordmark" x="100" y="152" text-anchor="middle">' +
             WORDMARK + '</text>');
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
    s.push("#" + OVERLAY_ID + " .ring,#" + OVERLAY_ID + " .bar,#" + OVERLAY_ID + " .swan{");
    s.push("fill:none;stroke-linecap:round;stroke-linejoin:round;");
    s.push("stroke-dasharray:1000;stroke-dashoffset:1000}");
    s.push("#" + OVERLAY_ID + " .ring{stroke:var(--p-mid,var(--p,#43c25e));stroke-width:2.6}");
    s.push("#" + OVERLAY_ID + " .bar{stroke:var(--p-mid,var(--p,#43c25e));stroke-width:3.6}");
    s.push("#" + OVERLAY_ID + " .swan{stroke:var(--p-hot,#7CFF9B);stroke-width:2.8}");
    // After .swan on purpose: same specificity, so the water reads as water.
    s.push("#" + OVERLAY_ID + " .wake{stroke:var(--p-mid,var(--p,#43c25e));stroke-width:2.2}");
    // The wordmark is FILLED, not stroked: it is type, and it fades up with the
    // swan beat rather than being drawn, because a dasharray on <text> animates
    // the letterforms' outlines, which is a different gesture entirely.
    s.push("#" + OVERLAY_ID + " .wordmark{fill:var(--p-hot,#7CFF9B);stroke:none;");
    s.push("font-family:inherit;font-size:15px;letter-spacing:3.2px;");
    s.push("opacity:0;transition:opacity 420ms linear}");
    s.push("#" + OVERLAY_ID + ".mark-on .wordmark{opacity:1}");
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
    s.push("#" + OVERLAY_ID + ".still .ring,#" + OVERLAY_ID + ".still .bar,#" +
           OVERLAY_ID + ".still .swan{stroke-dasharray:none;stroke-dashoffset:0}");
    s.push("#" + OVERLAY_ID + ".still .wordmark{opacity:1;transition:none}");
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
      if (reduced) root.className = "still";
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
        const rings = root.querySelectorAll(".ring");
        for (let i = 0; i < rings.length; i++) arm(rings[i], T_OCT + i * S_OCT, D_OCT);

        // Staggered per TRIGRAM, not per bar: thirty-six individually timed
        // strokes reads as static, not as a ring being drawn.
        const tris = root.querySelectorAll(".tri");
        for (let i = 0; i < tris.length; i++) {
          const bars = tris[i].children;
          for (let j = 0; j < bars.length; j++) arm(bars[j], T_RING + i * S_RING, D_RING);
        }

        const swan = root.querySelectorAll(".swan-g > *");
        for (let i = 0; i < swan.length; i++) arm(swan[i], T_SWAN + i * S_SWAN, D_SWAN);
        // The wordmark fades up as the last swan stroke finishes, so the mark
        // completes as one gesture rather than gaining a caption afterwards.
        timers.push(setTimeout(() => {
          if (root) root.classList.add("mark-on");
        }, T_SWAN + swan.length * S_SWAN + D_SWAN * 0.5));

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
  };
})(window);
