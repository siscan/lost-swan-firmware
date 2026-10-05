// A tiny DOM + virtual clock + event dispatcher, just big enough to run the
// REAL web/*.js files (bus.js, terminal.js, protocol.js, pearl.js, chat.js and,
// on request, bootanim.js) in a vm sandbox against the REAL terminal.html
// markup.  No npm, no browser.
//
// WHY IT EXISTS.  The pref layer is covered by test_toggles.js, but the
// station screen's BEHAVIOUR - what a keystroke does, who owns the keyboard,
// which ESC closes what - had no test at all, and that is where the bugs were:
// a focused button silently ate every key, a pending Y/N ate the N of PANEL,
// and one ESC closed the Pearl printout AND left protocol mode.  Every one of
// those is a property of event routing, which only running the real handlers
// against a real event path can show.
//
// What it is NOT: a layout engine.  Anything that depends on where pixels land
// (stacking, clipping, colours) still needs a browser.
//
// A step that waits on a Promise needs `await env.settle()`; a step that waits
// on time needs `env.flush(ms)`, which advances the virtual clock and runs every
// timer that falls due, in order.
"use strict";
const fs = require("fs");
const path = require("path");
const vm = require("vm");

const REPO = path.join(__dirname, "..", "..");
const WEB = path.join(REPO, "web");

// ---------------------------------------------------------------- clock ----
class VClock {
  constructor() { this.now = 1800000000000; this.seq = 0; this.timers = new Map(); }
  setTimeout(fn, ms, ...args) {
    const id = ++this.seq;
    this.timers.set(id, { at: this.now + Math.max(0, (ms | 0)), fn, args, every: 0 });
    return id;
  }
  setInterval(fn, ms, ...args) {
    const id = ++this.seq;
    ms = Math.max(1, ms | 0);
    this.timers.set(id, { at: this.now + ms, fn, args, every: ms });
    return id;
  }
  clear(id) { this.timers.delete(id); }
  advance(ms) {
    const end = this.now + ms;
    for (let guard = 0; guard < 2000000; guard++) {
      let best = null, bestId = 0;
      for (const [id, t] of this.timers) {
        if (t.at <= end && (!best || t.at < best.at || (t.at === best.at && id < bestId))) {
          best = t; bestId = id;
        }
      }
      if (!best) break;
      if (best.at > this.now) this.now = best.at;
      if (best.every) best.at += best.every; else this.timers.delete(bestId);
      best.fn(...best.args);
    }
    this.now = end;
  }
}

// ------------------------------------------------------------------ DOM ----
class Listeners {
  constructor() { this.list = []; }
  add(type, fn, opts) {
    const capture = opts === true || !!(opts && opts.capture);
    if (!this.list.some((l) => l.type === type && l.fn === fn && l.capture === capture)) {
      this.list.push({ type, fn, capture });
    }
  }
  remove(type, fn, opts) {
    const capture = opts === true || !!(opts && opts.capture);
    this.list = this.list.filter((l) => !(l.type === type && l.fn === fn && l.capture === capture));
  }
  get(type, capture) {
    return this.list.filter((l) => l.type === type && l.capture === capture).map((l) => l.fn);
  }
}

class TextNode {
  constructor(t) { this.nodeType = 3; this.data = String(t); this.parentNode = null; }
  get textContent() { return this.data; }
  set textContent(v) { this.data = String(v); }
}

function camel(s) { return s.replace(/-([a-z])/g, (_, c) => c.toUpperCase()); }

function makeStyle() {
  const st = { setProperty(k, v) { st[camel(k)] = v; st["--" + k] = v; } };
  return st;
}

const VOID = new Set(["br", "hr", "img", "input", "meta", "link"]);

function decode(s) {
  return s.replace(/&gt;/g, ">").replace(/&lt;/g, "<").replace(/&middot;/g, "\u00b7")
          .replace(/&hellip;/g, "\u2026").replace(/&amp;/g, "&").replace(/&nbsp;/g, " ");
}

class Element {
  constructor(tag, doc) {
    this.nodeType = 1;
    this.tagName = String(tag).toUpperCase();
    this.ownerDocument = doc;
    this.childNodes = [];
    this.parentNode = null;
    this._attrs = {};
    this._cls = "";
    this.dataset = {};
    this.style = makeStyle();
    this._ls = new Listeners();
    this.scrollTop = 0;
    this.scrollHeight = 0;
    this.offsetHeight = 48;
  }
  get id() { return this._attrs.id || ""; }
  set id(v) { this._attrs.id = String(v); }
  get className() { return this._cls; }
  set className(v) { this._cls = String(v); }
  get classList() {
    const el = this;
    const get = () => el._cls.split(/\s+/).filter(Boolean);
    const set = (a) => { el._cls = a.join(" "); };
    return {
      add(...n) { const a = get(); for (const x of n) if (!a.includes(x)) a.push(x); set(a); },
      remove(...n) { set(get().filter((x) => !n.includes(x))); },
      toggle(n, f) {
        const a = get(); const has = a.includes(n);
        const want = f === undefined ? !has : !!f;
        if (want && !has) a.push(n);
        if (!want && has) a.splice(a.indexOf(n), 1);
        set(a); return want;
      },
      contains(n) { return get().includes(n); },
    };
  }
  get children() { return this.childNodes.filter((c) => c.nodeType === 1); }
  get firstChild() { return this.childNodes[0] || null; }
  get lastChild() { return this.childNodes[this.childNodes.length - 1] || null; }
  get textContent() {
    return this.childNodes.map((c) => (c.nodeType === 3 ? c.data : c.textContent)).join("");
  }
  set textContent(v) {
    for (const c of this.childNodes) c.parentNode = null;
    this.childNodes = [];
    v = String(v);
    if (v) { const t = new TextNode(v); t.parentNode = this; this.childNodes.push(t); }
  }
  get innerHTML() { return this.textContent; }
  set innerHTML(html) {
    for (const c of this.childNodes) c.parentNode = null;
    this.childNodes = [];
    for (const n of parseHTML(html, this.ownerDocument)) this.appendChild(n);
  }
  appendChild(c) {
    if (c.parentNode) c.parentNode.removeChild(c);
    c.parentNode = this; this.childNodes.push(c); return c;
  }
  insertBefore(c, ref) {
    if (c.parentNode) c.parentNode.removeChild(c);
    const i = ref ? this.childNodes.indexOf(ref) : -1;
    c.parentNode = this;
    if (i < 0) this.childNodes.push(c); else this.childNodes.splice(i, 0, c);
    return c;
  }
  removeChild(c) {
    const i = this.childNodes.indexOf(c);
    if (i >= 0) { this.childNodes.splice(i, 1); c.parentNode = null; }
    return c;
  }
  remove() { if (this.parentNode) this.parentNode.removeChild(this); }
  setAttribute(k, v) {
    this._attrs[k] = String(v);
    if (k === "class") this._cls = String(v);
    if (k.startsWith("data-")) this.dataset[camel(k.slice(5))] = String(v);
  }
  getAttribute(k) { return k in this._attrs ? this._attrs[k] : null; }
  hasAttribute(k) { return k in this._attrs; }
  addEventListener(t, f, o) { this._ls.add(t, f, o); }
  removeEventListener(t, f, o) { this._ls.remove(t, f, o); }
  focus() { this.ownerDocument.activeElement = this; }
  blur() { if (this.ownerDocument.activeElement === this) this.ownerDocument.activeElement = null; }
  click() { this.ownerDocument.env.dispatch(this, { type: "click" }); }
  getBoundingClientRect() { return { left: 0, top: 0, right: 0, bottom: 0, width: 0, height: 0 }; }
  closest(sel) { for (let n = this; n && n.nodeType === 1; n = n.parentNode) if (matches(n, sel)) return n; return null; }
  matches(sel) { return matches(this, sel); }
  querySelectorAll(sel) {
    const out = [];
    const walk = (n) => {
      for (const c of n.childNodes) {
        if (c.nodeType !== 1) continue;
        if (matches(c, sel)) out.push(c);
        walk(c);
      }
    };
    walk(this);
    return out;
  }
  querySelector(sel) { return this.querySelectorAll(sel)[0] || null; }
  getElementById(id) { return this.querySelector("#" + id); }
}

function matchesCompound(el, sel) {
  const re = /(#[-\w]+)|(\.[-\w]+)|(\[[^\]]+\])|([a-zA-Z][-\w]*)|(\*)/g;
  let m; let any = false;
  while ((m = re.exec(sel))) {
    any = true;
    if (m[1] && el.id !== m[1].slice(1)) return false;
    if (m[2] && !el.classList.contains(m[2].slice(1))) return false;
    if (m[3]) {
      const body = m[3].slice(1, -1);
      const eq = body.indexOf("=");
      const k = eq < 0 ? body : body.slice(0, eq);
      const v = eq < 0 ? null : body.slice(eq + 1).replace(/^["']|["']$/g, "");
      if (k.startsWith("data-")) {
        const dk = camel(k.slice(5));
        if (!(dk in el.dataset)) return false;
        if (v !== null && el.dataset[dk] !== v) return false;
      } else {
        if (!el.hasAttribute(k)) return false;
        if (v !== null && el.getAttribute(k) !== v) return false;
      }
    }
    if (m[4] && el.tagName !== m[4].toUpperCase()) return false;
  }
  return any;
}

function matches(el, sel) {
  return sel.split(",").some((s) => {
    const parts = s.trim().split(/\s+/);
    if (!matchesCompound(el, parts[parts.length - 1])) return false;
    let n = el.parentNode;
    for (let i = parts.length - 2; i >= 0; i--) {
      while (n && !(n.nodeType === 1 && matchesCompound(n, parts[i]))) n = n.parentNode;
      if (!n) return false;
      n = n.parentNode;
    }
    return true;
  });
}

function parseHTML(html, doc) {
  html = String(html).replace(/<!--[\s\S]*?-->/g, "").replace(/<!DOCTYPE[^>]*>/i, "");
  const root = new Element("root", doc);
  const stack = [root];
  const re = /<\/?[a-zA-Z][^>]*>|[^<]+/g;
  let m;
  while ((m = re.exec(html))) {
    const tok = m[0];
    if (tok.startsWith("</")) { if (stack.length > 1) stack.pop(); continue; }
    if (tok.startsWith("<")) {
      const nm = /^<([a-zA-Z][-\w]*)/.exec(tok)[1].toLowerCase();
      const el = new Element(nm, doc);
      const are = /([a-zA-Z_:][-a-zA-Z0-9_:.]*)(?:\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+)))?/g;
      const attrText = tok.slice(1 + nm.length).replace(/\/?>$/, "");
      let a;
      while ((a = are.exec(attrText))) {
        const v = a[2] !== undefined ? a[2] : a[3] !== undefined ? a[3] : a[4] !== undefined ? a[4] : "";
        el.setAttribute(a[1], decode(v));
        if (a[1] === "style") {
          for (const kv of v.split(";")) {
            const i = kv.indexOf(":");
            if (i > 0) el.style[camel(kv.slice(0, i).trim())] = kv.slice(i + 1).trim();
          }
        }
      }
      stack[stack.length - 1].appendChild(el);
      if (!VOID.has(nm) && !/\/>$/.test(tok)) stack.push(el);
      continue;
    }
    const txt = decode(tok);
    if (/^\s*$/.test(txt)) continue;
    stack[stack.length - 1].appendChild(new TextNode(txt));
  }
  const out = root.childNodes.slice();
  for (const c of out) c.parentNode = null;
  return out;
}

// ------------------------------------------------------------------ env ----
function makeEnv(opts = {}) {
  const clock = new VClock();
  const RealDate = Date;
  class VDate extends RealDate {
    constructor(...a) { if (a.length === 0) super(clock.now); else super(...a); }
    static now() { return clock.now; }
  }

  const env = { clock, navigated: [], confirms: [], sockets: [], sent: [], logs: [], errors: [] };
  const win = {};
  win._ls = new Listeners();
  win.addEventListener = (t, f, o) => win._ls.add(t, f, o);
  win.removeEventListener = (t, f, o) => win._ls.remove(t, f, o);

  const doc = {
    env, nodeType: 9, _ls: new Listeners(), readyState: "loading",
    activeElement: null, fullscreenElement: null,
  };
  doc.addEventListener = (t, f, o) => doc._ls.add(t, f, o);
  doc.removeEventListener = (t, f, o) => doc._ls.remove(t, f, o);
  doc.createElement = (tag) => new Element(tag, doc);
  doc.createElementNS = (ns, tag) => new Element(tag, doc);
  doc.createTextNode = (t) => new TextNode(t);

  const html = new Element("html", doc);
  const head = new Element("head", doc);
  const body = new Element("body", doc);
  html.appendChild(head); html.appendChild(body);
  doc.documentElement = html; doc.head = head; doc.body = body;
  html.parentNode = doc;
  doc.getElementById = (id) => html.querySelector("#" + id);
  doc.querySelectorAll = (s) => html.querySelectorAll(s);
  doc.querySelector = (s) => html.querySelector(s);

  // The REAL page markup (scripts excluded).
  const page = fs.readFileSync(path.join(WEB, "terminal.html"), "utf8");
  const bodyHtml = /<body>([\s\S]*)<\/body>/.exec(page)[1].replace(/<script[\s\S]*?<\/script>/g, "");
  for (const n of parseHTML(bodyHtml, doc)) body.appendChild(n);

  // Event dispatch with capture / target / bubble semantics.
  env.dispatch = function (target, ev) {
    ev.target = target;
    ev.defaultPrevented = false;
    let stop = false, stopImm = false;
    ev.preventDefault = () => { ev.defaultPrevented = true; };
    ev.stopPropagation = () => { stop = true; };
    ev.stopImmediatePropagation = () => { stop = true; stopImm = true; };
    const chain = [];
    for (let n = target; n && n.nodeType === 1; n = n.parentNode) chain.push(n);
    const top = chain.slice().reverse();                  // html ... target
    const path_ = [win, doc, ...top];
    const run = (node, capture) => {
      for (const fn of node._ls.get(ev.type, capture)) {
        ev.currentTarget = node;
        try { fn.call(node, ev); } catch (e) { env.errors.push(e); }
        if (stopImm) break;
      }
    };
    for (let i = 0; i < path_.length - 1; i++) { run(path_[i], true); if (stop) return ev; }
    const tgt = path_[path_.length - 1];
    run(tgt, true); if (stopImm) return ev;
    run(tgt, false);
    if (stop) return ev;
    for (let i = path_.length - 2; i >= 0; i--) { run(path_[i], false); if (stop) return ev; }
    return ev;
  };
  env.key = function (key, o = {}) {
    const target = o.target || doc.activeElement || body;
    return env.dispatch(target, Object.assign({ type: "keydown", key, repeat: false,
      metaKey: false, ctrlKey: false, altKey: false }, o, { target: undefined }));
  };
  env.type = function (text, gapMs = 30) {
    for (const ch of text) { env.key(ch); clock.advance(gapMs); }
  };

  // The sandbox global.
  const store = opts.store || {};
  const sb = win;
  sb.window = sb;
  sb.document = doc;
  sb.Date = VDate;
  sb.setTimeout = (f, ms, ...a) => clock.setTimeout(f, ms, ...a);
  sb.setInterval = (f, ms, ...a) => clock.setInterval(f, ms, ...a);
  sb.clearTimeout = (id) => clock.clear(id);
  sb.clearInterval = (id) => clock.clear(id);
  sb.requestAnimationFrame = (f) => clock.setTimeout(() => f(clock.now), 16);
  sb.cancelAnimationFrame = (id) => clock.clear(id);
  sb.console = { log: (...a) => env.logs.push(a.join(" ")), warn: (...a) => env.logs.push("WARN " + a.join(" ")),
                 error: (...a) => env.errors.push(new Error(a.join(" "))) };
  sb.matchMedia = () => ({ matches: false });
  sb.confirm = (m) => { env.confirms.push(m); return env.confirmAnswer === undefined ? false : env.confirmAnswer; };
  sb.ResizeObserver = class { observe() {} };
  sb.location = {
    protocol: "http:", host: "lost.local",
    get href() { return "http://lost.local/terminal.html"; },
    set href(v) { env.navigated.push(v); },
  };
  sb.localStorage = opts.storageThrows
    ? { getItem() { throw new Error("SecurityError"); }, setItem() { throw new Error("SecurityError"); } }
    : { getItem: (k) => (k in store ? store[k] : null), setItem: (k, v) => { store[k] = String(v); },
        removeItem: (k) => { delete store[k]; } };
  env.store = store;
  sb.WebSocket = class {
    constructor(url) { this.url = url; this.readyState = 0; this.sent = []; env.sockets.push(this); }
    send(t) { this.sent.push(t); env.sent.push(t); }
    close() { this.readyState = 3; if (this.onclose) this.onclose(); }
  };
  env.fetchRoutes = opts.fetchRoutes || {};
  sb.fetch = (url, o) => {
    const h = env.fetchRoutes[url];
    if (!h) return Promise.reject(new Error("offline: " + url));
    return Promise.resolve(h(url, o));
  };
  sb.Blob = class { constructor(p, o) { this.p = p; this.o = o; } };
  sb.URL = { createObjectURL: () => "blob:x", revokeObjectURL() {} };
  sb.SwanFlap = {
    loadGlyphs: () => Promise.resolve(false),
    FlapDisplay: class { constructor() { this.primed = false; } setAll() {} setStates() {}
      reconcile() {} flipTo() {} spin() {} refresh() {} },
  };
  const ctx = vm.createContext(sb);
  env.win = win; env.doc = doc; env.body = body; env.ctx = ctx; env.VDate = VDate;

  env.load = (name) => {
    const f = path.join(WEB, name);
    vm.runInContext(fs.readFileSync(f, "utf8"), ctx, { filename: f });
  };
  env.ready = () => {
    doc.readyState = "interactive";
    for (const fn of doc._ls.get("DOMContentLoaded", false)) fn.call(doc, { type: "DOMContentLoaded" });
    doc.readyState = "complete";
  };
  env.openSocket = () => {
    const s = env.sockets[env.sockets.length - 1];
    s.readyState = 1; if (s.onopen) s.onopen();
    return s;
  };
  env.push = (obj) => {
    const s = env.sockets[env.sockets.length - 1];
    s.onmessage({ data: JSON.stringify(obj) });
  };
  env.lastSent = () => JSON.parse(env.sent[env.sent.length - 1] || "null");
  env.flush = (ms = 0) => clock.advance(ms);
  // Promise callbacks only run when the stack empties; await this between steps.
  env.settle = async () => { for (let i = 0; i < 12; i++) await new Promise((r) => setImmediate(r)); };
  env.run = (src) => vm.runInContext(src, ctx);
  return env;
}

// A state document with every field terminal.js reads.
function mkState(env, o = {}) {
  const nowS = Math.floor(env.clock.now / 1000);
  const target = o.target === undefined ? 0 : o.target;
  return {
    e: "state", t: env.clock.now, tz_offset_s: o.tz === undefined ? -25200 : o.tz,
    mode: o.mode || "clock",
    cd: { phase: o.phase || "idle", target, remaining_s: target ? target - nowS : 0,
          seconds_mode: o.seconds_mode || "seconds", seconds_live_s: o.live === undefined ? 240 : o.live,
          reveal_landed: !!o.reveal_landed },
    motion: Object.assign({ simulated: false, sim_columns: 0, disabled_columns: 0, maintenance: false }, o.motion || {}),
    audio: { volume: 70, mute: false, quiet_start_min: 0, quiet_end_min: 0, cues_present: 5, cues_total: 5 },
    time_valid: o.time_valid === undefined ? true : o.time_valid,
    cols: [0, 1, 2, 3, 4].map(() => ({ state: "IDLE", index: 0, mode: "real", retry: 0, cause: "none" })),
    cfg: Object.assign({ h24: true, zero_hold_s: 3, spin_s: 6, flaps_s_normal: 15 }, o.cfg || {}),
    sys: { rehome_retries: 3 },
  };
}

// Boot the page as a browser would: markup, then scripts in terminal.html order.
//   opts.realBoot  load the real bootanim_logo.js + bootanim.js (default: a stub
//                  that records each play() and the options it was given)
function boot(opts = {}) {
  const env = makeEnv(opts);
  env.load("bus.js");
  env.load("terminal.js");
  env.load("protocol.js");
  env.load("pearl.js");
  if (opts.realBoot) {
    env.load("bootanim_logo.js");
    env.load("bootanim.js");
  } else {
    env.run("window.__bootPlays = 0; window.__bootOpts = null; " +
            "window.SwanBoot = { play(o) { window.__bootPlays++; window.__bootOpts = o || null; return Promise.resolve(); } };");
  }
  env.load("chat.js");
  env.ready();
  return env;
}

module.exports = { makeEnv, mkState, boot, REPO, WEB };
