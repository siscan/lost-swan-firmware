// The split-flap display widget.
//
// ONE renderer, shared by the live web UI (app.js, driven by /ws), the
// presentation terminal (terminal.js) and the trace-replay simulator
// (sim/index.html).  All consume the same ring document and the same go/spin
// events, so what they show can never drift apart.
//
// The ring document may be either data/ring.json or GET /api/ring: the first
// carries a shared `slots` array with per-column overrides, the second gives
// every column its own `ring`.  colTable() accepts both.
"use strict";

(function (global) {
  const FALLBACK_SCHEME = {
    card: { default: "#181818" },
    ink: { default: "#e8e4da", glyph: "#b03a2e" },
  };

  // Injected symbol ids are namespaced.  glyphs.svg carries the bare manifest
  // names by design, but those are generic - `sun`, `hand`, `gate`, `wave` -
  // and once injected they live in the page's single id space, where a clash
  // with a page element would silently render the wrong picture.
  const ID_PREFIX = "swan-glyph-";
  const CONTAINER_ID = "swan-glyph-defs";
  let available = null;   // Set of glyph ids, or null until a sheet is loaded

  // Fetch the sheet once and inject it, so every <use> is a SAME-DOCUMENT
  // reference.  External references (<use href="sheet.svg#id">) are not
  // supported in WebKit at all and fail over file://, which would mean a
  // display that works on desktop Chrome and is blank on an iPhone.
  //
  // Resolves false when the sheet cannot be loaded (file:// in the simulator,
  // or a board whose LittleFS predates it); callers keep the text labels.
  function loadGlyphs(url) {
    if (available) return Promise.resolve(true);
    return fetch(url)
      .then((r) => (r.ok ? r.text() : Promise.reject(new Error("HTTP " + r.status))))
      .then((text) => {
        let host = document.getElementById(CONTAINER_ID);
        if (!host) {
          host = document.createElement("div");
          host.id = CONTAINER_ID;
          host.setAttribute("aria-hidden", "true");
          host.style.display = "none";
          document.body.appendChild(host);
        }
        host.innerHTML = text;
        const ids = new Set();
        host.querySelectorAll("symbol[id]").forEach((sym) => {
          const name = sym.id;
          sym.id = ID_PREFIX + name;
          ids.add(name);
        });
        if (ids.size === 0) return false;
        available = ids;
        return true;
      })
      .catch(() => false);
  }

  function hasGlyph(id) {
    return available !== null && available.has(id);
  }

  const SVG_NS = "http://www.w3.org/2000/svg";
  const XLINK_NS = "http://www.w3.org/1999/xlink";

  class FlapDisplay {
    // host: a container element.  opts.gapAfter inserts the physical 3 + 2
    // band gap (spec 1) after that column index.
    constructor(host, ring, opts) {
      this.host = host;
      this.opts = Object.assign({ gapAfter: 2, onFace: null }, opts || {});
      this.cols = [];
      this.setRing(ring);
    }

    setRing(ring) {
      this.ring = ring || { slot_count: 50, columns: [] };
      this.n = this.ring.slot_count || 50;
      const count = (this.ring.columns || []).length || 5;
      if (this.cols.length !== count) this.build(count);
      for (let i = 0; i < this.cols.length; i++) this.paint(i, this.cols[i].idx || 0);
    }

    // Called after loadGlyphs() resolves, to swap text placeholders for the
    // real artwork without rebuilding anything.
    refresh() {
      for (let i = 0; i < this.cols.length; i++) this.paint(i, this.cols[i].idx || 0);
    }

    build(count) {
      this.cols.forEach((c) => clearInterval(c.timer));
      this.host.textContent = "";
      this.cols = [];
      for (let i = 0; i < count; i++) {
        if (i === this.opts.gapAfter + 1) {
          const gap = document.createElement("div");
          gap.className = "flap-gap";
          this.host.appendChild(gap);
        }
        const col = document.createElement("div");
        col.className = "flap-col";
        const card = document.createElement("div");
        card.className = "flap-card";

        // Both faces exist for the life of the card and are toggled; building
        // an element per flip would churn through hundreds of nodes during a
        // 49-flip wrap.
        const text = document.createElement("span");
        text.className = "flap-text";
        const svg = document.createElementNS(SVG_NS, "svg");
        svg.setAttribute("class", "flap-glyph");
        // The 100:127 box is the flap card's own ratio and every glyph is
        // pre-scaled inside it, so relative sizes match the printed cards.
        // Default preserveAspectRatio (xMidYMid meet) - never stretch.
        svg.setAttribute("viewBox", "0 0 100 127");
        svg.setAttribute("aria-hidden", "true");
        const use = document.createElementNS(SVG_NS, "use");
        svg.appendChild(use);

        card.appendChild(text);
        card.appendChild(svg);
        col.appendChild(card);
        this.host.appendChild(col);
        this.cols.push({ card, text, svg, use, idx: 0, timer: null, target: -1, href: "" });
      }
    }

    colTable(i) {
      const c = (this.ring.columns || [])[i] || {};
      return c.ring || c.slots || this.ring.slots || [];
    }

    scheme(i) {
      const c = (this.ring.columns || [])[i] || {};
      return (this.ring.schemes || {})[c.scheme] || FALLBACK_SCHEME;
    }

    // How a slot reads on a card.  Digits, AM/PM and blank are text; glyphs
    // and the WiFi mark come from glyphs.svg when it loaded, and fall back to
    // the manifest name otherwise - which is also what makes a table/drum
    // mismatch obvious on the Calibrate walk (spec 4).
    static face(slot) {
      if (!slot) return { text: "?", small: true, glyph: null };
      if (slot.cat === "blank") return { text: "", small: false, glyph: null };
      if (slot.cat === "digit" || slot.cat === "ampm") {
        return { text: slot.id, small: slot.id.length > 1, glyph: null };
      }
      return { text: slot.id, small: true, glyph: hasGlyph(slot.id) ? slot.id : null };
    }

    paint(i, idx) {
      const col = this.cols[i];
      if (!col) return;
      const slot = this.colTable(i)[idx];
      const s = this.scheme(i);
      const isGlyph = slot && (slot.cat === "glyph" || slot.cat === "wifi");
      const f = FlapDisplay.face(slot);

      col.card.style.background = (isGlyph && s.card.glyph) || s.card.default;
      // The glyph artwork is fill="currentColor", so the card's colour drives
      // both the text placeholders and the real thing: red on black for the
      // minutes group, black on white/red for the seconds group.
      col.card.style.color = (isGlyph && s.ink.glyph) || s.ink.default;

      if (f.glyph) {
        const href = "#" + ID_PREFIX + f.glyph;
        if (col.href !== href) {
          col.use.setAttributeNS(null, "href", href);
          // Safari before 12 only honours the xlink form; harmless elsewhere.
          col.use.setAttributeNS(XLINK_NS, "xlink:href", href);
          col.href = href;
        }
        col.svg.style.display = "";
        col.text.style.display = "none";
      } else {
        col.svg.style.display = "none";
        col.text.style.display = "";
        col.text.textContent = f.text;
        col.text.classList.toggle("small", f.small);
      }
      col.card.title = slot ? slot.label || slot.id : "slot " + idx;
      col.idx = idx;
      if (this.opts.onFace) this.opts.onFace(i, idx, slot);
    }

    // Animate (to - cur) mod N forward flips - the ring is one-way, so there
    // is never a shorter way round and never a reverse.
    flipTo(i, to, flaps) {
      const col = this.cols[i];
      if (!col || to < 0 || to >= this.n) return;
      // The scheduler can issue the same target twice in one tick (a frame
      // and then the convergence pass, before the axis has reported Moving).
      // Restarting the animation on the duplicate would stutter the card.
      if (col.timer && col.target === to) return;
      const per = Math.max(28, 1000 / (flaps || 15));
      clearInterval(col.timer);
      col.target = to;
      if (col.idx === to) {
        col.timer = null;
        return;
      }
      col.timer = setInterval(() => {
        if (col.idx === to) {
          clearInterval(col.timer);
          col.timer = null;
          return;
        }
        const next = (col.idx + 1) % this.n;
        col.card.classList.add("flip");
        setTimeout(() => {
          this.paint(i, next);
          col.card.classList.remove("flip");
        }, 20);
      }, per);
    }

    spin(i, flaps, secs) {
      const col = this.cols[i];
      if (!col) return;
      const per = Math.max(28, 1000 / (flaps || 25));
      clearInterval(col.timer);
      col.target = -1;  // open loop: the landing index is unknown
      let left = (secs * 1000) / per;
      col.timer = setInterval(() => {
        col.card.classList.add("flip");
        const next = (col.idx + 1) % this.n;
        setTimeout(() => {
          this.paint(i, next);
          col.card.classList.remove("flip");
        }, 18);
        if (--left <= 0) {
          clearInterval(col.timer);
          col.timer = null;
        }
      }, per);
    }

    // Snap without animating: used when a fresh page load has to catch up with
    // whatever the columns are already showing.
    setAll(indices) {
      for (let i = 0; i < this.cols.length && i < indices.length; i++) {
        clearInterval(this.cols[i].timer);
        this.cols[i].timer = null;
        this.cols[i].target = -1;
        if (indices[i] >= 0) this.paint(i, indices[i]);
      }
    }

    reset() {
      this.cols.forEach((c, i) => {
        clearInterval(c.timer);
        c.timer = null;
        c.target = -1;
        this.paint(i, 0);
      });
    }
  }

  global.SwanFlap = { FlapDisplay, loadGlyphs, hasGlyph };
})(window);
