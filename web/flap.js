// The split-flap display widget.
//
// ONE renderer, shared by the live web UI (app.js, driven by /ws) and the
// trace-replay simulator (sim/index.html, driven by recorded traces).  Both
// consume the same ring document shape and the same go/spin events, so what
// the simulator shows and what the UI shows can never drift apart.
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

  class FlapDisplay {
    // host: a container element.  opts.gapAfter inserts the physical 3 + 2
    // band gap (spec 1) after that column index; opts.size scales the cards.
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
        col.appendChild(card);
        this.host.appendChild(col);
        this.cols.push({ card: card, idx: 0, timer: null, target: -1 });
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

    // How a slot reads on a card.  Glyphs have no glyph font yet, so they
    // render as their manifest name - which is also what makes a table/drum
    // mismatch obvious on the Calibrate walk (spec 4).
    static face(slot) {
      if (!slot) return { text: "?", small: true };
      if (slot.cat === "blank") return { text: "", small: false };
      if (slot.cat === "digit" || slot.cat === "ampm") {
        return { text: slot.id, small: slot.id.length > 1 };
      }
      if (slot.cat === "wifi") return { text: "☷", small: false };
      return { text: slot.id, small: true };
    }

    paint(i, idx) {
      const col = this.cols[i];
      if (!col) return;
      const table = this.colTable(i);
      const slot = table[idx];
      const s = this.scheme(i);
      const glyph = slot && slot.cat === "glyph";
      const f = FlapDisplay.face(slot);
      col.card.style.background = (glyph && s.card.glyph) || s.card.default;
      col.card.style.color = (glyph && s.ink.glyph) || s.ink.default;
      col.card.textContent = f.text;
      col.card.classList.toggle("small", f.small);
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
      if (col.idx === to) return;
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
        if (--left <= 0) clearInterval(col.timer);
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

  global.SwanFlap = { FlapDisplay: FlapDisplay };
})(window);
