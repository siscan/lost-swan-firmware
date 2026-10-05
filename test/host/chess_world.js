// Support for the two chess suites (test_chess_engine.js, test_chess_ui.js).
//
// makeWorld(): a just-big-enough browser for web/chess.js + web/protocol.js + the
// REAL bindKeyboard() lifted out of web/terminal.js.  Virtual time, virtual
// events.  (web_harness.js is the other fake DOM in this directory, for the
// station screen; this one predates it and drives chess through its own seam.)
//
// loadChess() / seamTransform(): chess.js with ONE export line added to an
// in-memory copy, so a test can reach the engine's internals - the move
// generator, perft, the game state.  The repo file is never written.  If the
// anchor line in chess.js moves, seamTransform throws, and that is the point.
"use strict";
const fs = require("fs");
const path = require("path");
const vm = require("vm");

const WEB = path.join(__dirname, "..", "..", "web") + "/";
const SRC_PATH = WEB + "chess.js";

class Text {
  constructor(t) { this.nodeType = 3; this.data = String(t); this.parentNode = null; }
  get textContent() { return this.data; }
}
function decode(s) { return s.replace(/&gt;/g, ">").replace(/&lt;/g, "<").replace(/&amp;/g, "&"); }

class El {
  constructor(tag) {
    this.nodeType = 1; this.tagName = String(tag).toUpperCase(); this.parentNode = null;
    this.childNodes = []; this.dataset = {}; this.style = {}; this._attrs = {}; this._cls = new Set();
    this._ls = {}; this.onclick = null; this.scrollTop = 0; this.scrollHeight = 0;
    const self = this;
    this.classList = {
      add() { for (const c of arguments) self._cls.add(c); },
      remove() { for (const c of arguments) self._cls.delete(c); },
      contains(c) { return self._cls.has(c); },
      toggle(c, f) { const on = f === undefined ? !self._cls.has(c) : !!f; if (on) self._cls.add(c); else self._cls.delete(c); return on; }
    };
  }
  get className() { return Array.from(this._cls).join(" "); }
  set className(v) { this._cls = new Set(String(v).split(/\s+/).filter(Boolean)); }
  get id() { return this._attrs.id || ""; }
  set id(v) { this._attrs.id = String(v); }
  get firstChild() { return this.childNodes[0] || null; }
  get lastChild() { return this.childNodes[this.childNodes.length - 1] || null; }
  get textContent() { return this.childNodes.map((c) => c.textContent).join(""); }
  set textContent(v) {
    this.childNodes.forEach((c) => { c.parentNode = null; });
    this.childNodes = []; v = String(v);
    if (v) { const t = new Text(v); t.parentNode = this; this.childNodes.push(t); }
  }
  set innerHTML(html) {
    this.childNodes.forEach((c) => { c.parentNode = null; }); this.childNodes = [];
    const re = /<(\/?)([a-zA-Z0-9]+)((?:\s+[^>]*?)?)>|([^<]+)/g; const stack = [this]; let m;
    while ((m = re.exec(html))) {
      const top = stack[stack.length - 1];
      if (m[4] !== undefined) { const t = new Text(decode(m[4])); t.parentNode = top; top.childNodes.push(t); continue; }
      if (m[1] === "/") { if (stack.length > 1) stack.pop(); continue; }
      const el = new El(m[2]);
      m[3].replace(/([a-zA-Z-]+)="([^"]*)"/g, (_, k, v) => { el.setAttribute(k, v); return ""; });
      top.appendChild(el); stack.push(el);
    }
  }
  appendChild(c) { if (c.parentNode) c.parentNode.removeChild(c); c.parentNode = this; this.childNodes.push(c); return c; }
  removeChild(c) { const i = this.childNodes.indexOf(c); if (i >= 0) { this.childNodes.splice(i, 1); c.parentNode = null; } return c; }
  setAttribute(k, v) { this._attrs[k] = String(v); if (k === "class") this.className = v; }
  getAttribute(k) { return this._attrs[k] === undefined ? null : this._attrs[k]; }
  addEventListener(type, fn) { (this._ls[type] = this._ls[type] || []).push(fn); }
  closest(sel) {
    for (let n = this; n && n.nodeType === 1; n = n.parentNode) if (sel[0] === "." && n._cls.has(sel.slice(1))) return n;
    return null;
  }
  querySelector(sel) {
    const stack = this.childNodes.slice();
    while (stack.length) {
      const n = stack.shift(); if (n.nodeType !== 1) continue;
      if ((sel[0] === "#" && n.id === sel.slice(1)) || (sel[0] === "." && n._cls.has(sel.slice(1)))) return n;
      stack.unshift(...n.childNodes);
    }
    return null;
  }
  getContext() { return { createImageData: (w, h) => ({ data: new Uint8ClampedArray(w * h * 4) }), putImageData() {} }; }
  contains(n) { for (; n; n = n.parentNode) if (n === this) return true; return false; }
}

function makeWorld(opts) {
  opts = opts || {};
  const W = { termSaw: [], clicks: [], sent: [], boot: [], reducedMotion: !!opts.reducedMotion, errors: [] };

  // ---- virtual time
  let now = 1000000, seq = 0; const timers = new Map();
  const add = (fn, ms, every) => { const id = ++seq; timers.set(id, { id, at: now + Math.max(0, ms | 0), fn, every: every || 0 }); return id; };
  W.advance = function (ms) {
    const end = now + ms;
    for (;;) {
      let next = null;
      for (const t of timers.values()) if (t.at <= end && (!next || t.at < next.at || (t.at === next.at && t.id < next.id))) next = t;
      if (!next) break;
      now = next.at;
      if (next.every) next.at += next.every; else timers.delete(next.id);
      try { next.fn(); } catch (e) { W.errors.push(String(e && e.stack || e)); }
    }
    now = end;
  };
  W.timerCount = () => timers.size;
  W.now = () => now;

  // ---- DOM
  const html = new El("html"), head = new El("head"), body = new El("body"), screen = new El("div");
  html.appendChild(head); html.appendChild(body); screen.id = "screen"; body.appendChild(screen);
  const L = { win: { cap: [], bub: [] }, doc: { cap: [], bub: [] } };
  const reg = (bucket) => (type, fn, o) => {
    if (type !== "keydown") return;
    const cap = o === true || (o && o.capture === true);
    (cap ? bucket.cap : bucket.bub).push(fn);
  };
  const documentObj = {
    readyState: "complete", head, body, documentElement: html,
    createElement: (t) => new El(t), createTextNode: (t) => new Text(t),
    getElementById: (id) => { if (id === "screen") return screen; return body.querySelector("#" + id) || head.querySelector("#" + id); },
    addEventListener: reg(L.doc), querySelectorAll: () => [], fullscreenElement: null
  };
  const sandbox = {
    console, setTimeout: (f, m) => add(f, m, 0), setInterval: (f, m) => add(f, m, m || 1),
    clearTimeout: (id) => timers.delete(id), clearInterval: (id) => timers.delete(id),
    document: documentObj, addEventListener: reg(L.win), localStorage: { getItem: () => null, setItem() {} },
    location: { href: "" }, matchMedia: () => ({ matches: W.reducedMotion }), __vnow: () => now
  };
  sandbox.window = sandbox;

  // ---- SwanTerm stub (the surface protocol.js and chess.js use)
  const subs = {};
  const term = {
    state: null, prefs: { protocol: false, egg: false, click: true, crt: false, dock: true },
    _station: opts.station || "flame", _phase: "idle", _target: null,
    station() { return this._station; },
    setStation(n) { if (n === this._station) return; this._station = n; this.emit("station", n); },
    savePref() {}, clickSound(k) { W.clicks.push(k); },
    phase() { return this._phase; }, remaining() { return this._target; },
    secondsLive() { return 240; }, shownS(r) { return Math.ceil(r); }, timeValid() { return true; },
    send(c, p) { W.sent.push([c, p]); },
    applyPrefs() { if (sandbox.SwanProtocol) sandbox.SwanProtocol.apply(); this.emit("prefs", this.prefs); },
    on(evt, fn) { (subs[evt] = subs[evt] || []).push(fn); return this; },
    emit(evt, arg) { (subs[evt] || []).slice().forEach((f) => f(arg)); }
  };
  sandbox.SwanTerm = term;
  sandbox.SwanBoot = { play(o) { W.boot.push(o); return Promise.resolve(); } };
  sandbox.SwanPearl = { isOpen: () => false, open() { W.pearlOpened = true; }, close() {} };
  sandbox.SwanChat = { isOpen: () => false, feedKey() {} };

  const ctx = vm.createContext(sandbox);
  vm.runInContext("Date.now = function () { return __vnow(); };", ctx);
  vm.runInContext("(function(){ var s = 4242; Math.random = function () { s = (Math.imul(s, 1664525) + 1013904223) >>> 0; return s / 4294967296; }; })();", ctx);

  // the REAL terminal.js key handler, lifted by source extraction
  const tsrc = fs.readFileSync(WEB + "terminal.js", "utf8");
  const at = tsrc.indexOf("function bindKeyboard()");
  let depth = 0, end = -1;
  for (let i = tsrc.indexOf("{", at); i < tsrc.length; i++) { if (tsrc[i] === "{") depth++; else if (tsrc[i] === "}" && --depth === 0) { end = i + 1; break; } }
  const bk = tsrc.slice(at, end);
  sandbox.press = (k) => W.termSaw.push(k);
  sandbox.doCancel = () => W.termSaw.push("CANCEL");
  sandbox.clickSound = () => {};
  vm.runInContext(bk + "\nbindKeyboard();", ctx);

  W.sandbox = sandbox; W.ctx = ctx; W.term = term; W.body = body; W.html = html; W.L = L;
  W.loadFile = (name, transform) => {
    let text = fs.readFileSync(WEB + name, "utf8");
    if (transform) text = transform(text);
    vm.runInContext(text, ctx, { filename: name });
  };
  W.key = function (key, o) {
    o = o || {};
    const ev = {
      type: "keydown", key, repeat: !!o.repeat, ctrlKey: !!o.ctrl, metaKey: !!o.meta, altKey: !!o.alt,
      target: o.target || body, defaultPrevented: false, _sp: false, _sip: false,
      preventDefault() { this.defaultPrevented = true; },
      stopPropagation() { this._sp = true; },
      stopImmediatePropagation() { this._sp = true; this._sip = true; }
    };
    for (const list of [L.win.cap, L.doc.cap, L.doc.bub, L.win.bub]) {
      for (const fn of list.slice()) { try { fn(ev); } catch (e) { W.errors.push(String(e && e.stack || e)); } if (ev._sip) break; }
      if (ev._sp) break;
    }
    return ev;
  };
  W.type = function (s, gapMs) { const out = []; for (const ch of s) { out.push(W.key(ch)); W.advance(gapMs === undefined ? 30 : gapMs); } return out; };
  W.click = function (el) {
    const ev = { type: "click", target: el, currentTarget: null, defaultPrevented: false, _sp: false,
      preventDefault() { this.defaultPrevented = true; }, stopPropagation() { this._sp = true; } };
    for (let n = el; n; n = n.parentNode) {
      ev.currentTarget = n;
      try {
        if (typeof n.onclick === "function") n.onclick(ev);
        for (const fn of (n._ls.click || []).slice()) fn(ev);
      } catch (e) { W.errors.push(String(e && e.stack || e)); }
      if (ev._sp) break;
    }
    return ev;
  };
  return W;
}

// ---- chess.js with its internals exposed ------------------------------------
function seamTransform(raw) {
  const seam = "root.SwanChess = api;";
  if (raw.split(seam).length !== 2) throw new Error("seam line not unique/absent in chess.js");
  const exportLine =
    " root.__eng = { fromFen, legal, make, perft, statusOf, pickMove, inCheck, attacked," +
    " kingSq, sqName, nameSq, insufficient, evalMat, search, moveText, pseudo, START_FEN," +
    " colorOf, other, fromFenRaw: fromFen," +
    " get game() { return game; }, set game(v) { game = v; }," +
    " get el() { return el; }, get menuOn() { return menuOn; }, get entry() { return entry; }," +
    " get timers() { return timers; }," +
    " newGame, render, onSquare, applyPlayer, engineMove, finished, undo, openMenu, menuKey," +
    " runCode, typeOut, incursion, handoff, askPromo, build };";
  return raw.replace(seam, seam + exportLine);
}

// The engine alone, in a bare context (no DOM): { ctx, eng, api, src }.
function loadChess(sandbox) {
  const raw = fs.readFileSync(SRC_PATH, "utf8");
  const text = seamTransform(raw);
  const ctx = vm.createContext(sandbox || { console });
  vm.runInContext(text, ctx, { filename: "chess.js(seamed-copy)" });
  return { ctx, eng: ctx.__eng, api: ctx.SwanChess, src: raw };
}

module.exports = { makeWorld, El, loadChess, seamTransform, SRC_PATH };
