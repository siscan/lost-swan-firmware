// Chat mode (spec §15 phase 7, the show-accuracy pack).
//
// Lostpedia's account of Michael's session is the whole brief: a burst of
// key-mashing at the idle prompt, and a chat window that was not there before.
// So the trigger is the mash, not a button - a button would be a feature, and
// this is meant to be found.
//
// SCREEN-SIDE ONLY.  Nothing here sends a command, and the only fetch is
// chat.json.  The flaps never learn this happened.
//
// The script lives in web/chat.json so the lines can be replaced by dropping in
// one file.  The built-in copy below is the fallback and is deliberately short:
// a display with no chat.json, or with a chat.json somebody has just broken,
// must still be a display.
//
// No backslash escapes anywhere in this file's string literals, deliberately -
// a raw newline smuggled into one has blanked this UI three times (CLAUDE.md).
"use strict";

(function () {
  // ------------------------------------------------------------------------
  // THE MASH DETECTOR
  //
  // N = 12 distinct character keys inside W = 1200 ms, in a window containing
  // NO digit and NO separator.
  //
  // The disqualifier is the load-bearing half.  The Numbers are digits and
  // spaces and nothing else, so a digit or a space CLEARS the buffer outright:
  // entering the Numbers cannot arm this by bad luck, it cannot arm it at all.
  // That leaves prose as the only accidental source, and 12 DISTINCT characters
  // in 1.2 s is 10 unique characters a second - typing at 120 wpm yields about
  // eight, because prose repeats itself.  A two-handed slap across two rows
  // clears it on the first try, which is the point: hard by accident, easy on
  // purpose.
  //
  // Held keys are ignored (ev.repeat), the same lesson terminal.js learned when
  // a leaned-on digit filled the entry buffer.
  const MASH_N = 12;
  const MASH_W_MS = 1200;

  // A mash is a physical act and the fingers are still moving when the window
  // opens; without this, ESC re-opens the window it just closed.
  const REARM_MS = 1200;

  // chat.json is served by the single httpd task, which an OTA can hold for up
  // to 120 s (spec §10.4).  The egg waits this long and then plays the built-in
  // rather than sitting on an empty window for two minutes.
  const FETCH_MS = 2500;

  const MAX_LINES = 64;
  const MAX_TEXT = 240;
  const DEFAULT_CPS = 17;
  const DEFAULT_PAUSE = 800;

  // The fallback, written in chat.json's OWN schema so the two cannot drift
  // apart in shape and this doubles as documentation of the format.  It goes
  // through the same sanitize() the uploaded file does.
  const BUILT_IN = {
    version: 1,
    cps: DEFAULT_CPS,
    lines: [
      { who: "them", text: "is someone there", pause_ms: 1600 },
      { who: "you", text: "who is this", pause_ms: 1200 },
      { who: "them", text: "they watch the keys", pause_ms: 1900 },
      { who: "you", text: "where are you", pause_ms: 2600 },
      { who: "them", text: "dont push it", pause_ms: 2400 },
      { who: "you", text: "push what", pause_ms: 3000 },
      { who: "sys", text: "connection terminated", pause_ms: 0 },
    ],
  };

  const WHO = { them: 1, you: 1, sys: 1 };
  const PREFIX = { them: "", you: ">", sys: "" };

  // ------------------------------------------------------------------------
  // Host surface.  Resolved lazily: chat.js must not care whether it loaded
  // before or after terminal.js, and must not throw on a page that has neither.
  // ------------------------------------------------------------------------
  const term = () => window.SwanTerm || null;

  function eggArmed() {
    const t = term();
    if (!t || !t.prefs || !t.prefs.egg) return false;
    // SWAN'S EGG (2026-08-25).  The Pearl and the Flame have their own screens
    // and their own openings; a ghost story arriving over either of them would
    // be another station's content leaking in.
    if (t.station && t.station() !== "swan") return false;
    // Idle only.  A countdown at 3:12 with people watching it is not the moment
    // to cover the readout with a ghost story.
    return typeof t.phase === "function" && t.phase() === "idle";
  }

  function click(kind) {
    const t = term();
    if (t && typeof t.clickSound === "function") t.clickSound(kind);
  }

  const reduced = () => !!(window.matchMedia &&
      window.matchMedia("(prefers-reduced-motion: reduce)").matches);

  // ------------------------------------------------------------------------
  // Styles, injected once.  --p-mid and --p-bg are NOT defined in terminal.css
  // today, so the var() fallbacks here are load-bearing rather than decorative.
  //
  // Joined with a space, not a newline: CSS does not care, and the file then
  // needs no escape at all.
  // ------------------------------------------------------------------------
  const CSS = [
    ".swan-chat { position: fixed; inset: 0; z-index: 40; display: flex;",
    "  align-items: center; justify-content: center;",
    "  padding: clamp(10px, 3vmin, 40px);",
    "  opacity: 0; transition: opacity 160ms linear; }",
    ".swan-chat.up { opacity: 1; }",
    ".swan-chat-bd { position: absolute; inset: 0;",
    "  background: var(--p-bg, #04120a); opacity: 0.94; }",
    ".swan-chat-win { position: relative; display: flex; flex-direction: column;",
    "  width: min(72ch, 100%); max-height: 100%;",
    "  border: 1px solid var(--p-dim, #2f7a3a);",
    "  padding: clamp(10px, 2vmin, 26px); gap: clamp(6px, 1.2vmin, 16px);",
    "  font-size: clamp(13px, 2.1vmin, 26px); line-height: 1.6; }",
    ".swan-chat-hd, .swan-chat-ft { flex: 0 0 auto;",
    "  font-size: clamp(9px, 1.3vmin, 15px); letter-spacing: 0.28em;",
    "  text-transform: uppercase; color: var(--p-dim, #2f7a3a); }",
    ".swan-chat-hd { border-bottom: 1px solid var(--p-dim, #2f7a3a);",
    "  padding-bottom: 0.5em; }",
    ".swan-chat-ft { border-top: 1px solid var(--p-dim, #2f7a3a);",
    "  padding-top: 0.5em; }",
    ".swan-chat-log { flex: 1 1 auto; min-height: 0; overflow-y: auto;",
    "  overflow-x: hidden; -webkit-overflow-scrolling: touch; }",
    ".swan-chat-row { display: flex; gap: 0.6em; margin: 0.35em 0;",
    "  word-break: break-word; }",
    ".swan-chat-pre { flex: 0 0 auto; min-width: 1ch;",
    "  color: var(--p-dim, #2f7a3a); }",
    ".swan-chat-them .swan-chat-t { color: var(--p-mid, var(--p, #43c25e)); }",
    ".swan-chat-you .swan-chat-t { color: var(--p-hot, #7CFF9B); }",
    ".swan-chat-sys { justify-content: center; }",
    ".swan-chat-sys .swan-chat-pre { display: none; }",
    ".swan-chat-sys .swan-chat-t { color: var(--p-dim, #2f7a3a);",
    "  letter-spacing: 0.28em; }",
    ".swan-chat-cur { display: inline-block; width: 0.55em; height: 1.05em;",
    "  vertical-align: -0.15em; background: var(--p-hot, #7CFF9B);",
    "  animation: swan-chat-blink 1.06s steps(1, end) infinite; }",
    "@keyframes swan-chat-blink { 0%, 49% { opacity: 1; } 50%, 100% { opacity: 0; } }",
    "@media (prefers-reduced-motion: reduce) {",
    "  .swan-chat { transition: none; }",
    "  .swan-chat-cur { animation: none; } }",
  ].join(" ");

  function ensureStyles() {
    if (document.getElementById("swan-chat-css")) return;
    const s = document.createElement("style");
    s.id = "swan-chat-css";
    s.textContent = CSS;
    document.head.appendChild(s);
  }

  // ------------------------------------------------------------------------
  // The script: validated defensively, because chat.json is a file the owner
  // edits by hand.  Anything unreadable is dropped, never rendered.
  // ------------------------------------------------------------------------
  // Own-property test, never truthiness: a bare object inherits from
  // Object.prototype, so a who of "constructor" would read as a valid role and
  // hand back a FUNCTION where a prefix string was expected.
  const has = (o, k) => Object.prototype.hasOwnProperty.call(o, k);

  function clampNum(v, lo, hi, dflt) {
    const n = (typeof v === "number" && isFinite(v)) ? v : dflt;
    return Math.min(hi, Math.max(lo, n));
  }

  // One entry is one row.  A smuggled newline or NUL would not break the row -
  // it would break the per-character cursor arithmetic underneath it - so every
  // control character becomes a space.  Done by code point rather than by regex
  // class so this file needs no escape sequence.
  function stripControls(s) {
    let out = "";
    for (let i = 0; i < s.length && out.length < MAX_TEXT; i++) {
      const c = s.charCodeAt(i);
      out += (c < 32 || c === 127) ? " " : s.charAt(i);
    }
    return out.trim();
  }

  function sanitize(doc) {
    if (!doc || typeof doc !== "object" || Array.isArray(doc)) return null;
    if (doc.version !== 1) return null;      // a future schema is not ours to guess at
    if (!Array.isArray(doc.lines)) return null;
    const cps = clampNum(doc.cps, 1, 200, DEFAULT_CPS);
    const out = [];
    for (let i = 0; i < doc.lines.length && out.length < MAX_LINES; i++) {
      const raw = doc.lines[i];
      if (!raw || typeof raw !== "object") continue;
      if (typeof raw.text !== "string") continue;
      const text = stripControls(raw.text);
      if (!text) continue;
      out.push({
        who: has(WHO, raw.who) ? raw.who : "them",
        text,
        pause_ms: clampNum(raw.pause_ms, 0, 10000, DEFAULT_PAUSE),
      });
    }
    return out.length ? { cps, lines: out } : null;
  }

  let script = null;    // validated, cached for the life of the page
  let pending = null;   // the in-flight load, so a double open fetches once

  function loadScript() {
    if (script) return Promise.resolve(script);
    if (pending) return pending;
    const slow = new Promise((res) => setTimeout(() => res(null), FETCH_MS));
    const got = fetch("chat.json")
      .then((r) => (r.ok ? r.json() : null))
      .catch(() => null);                    // offline, 404, or not JSON at all
    pending = Promise.race([got, slow]).then((doc) => {
      const ok = sanitize(doc);
      // The only way the owner learns their replacement was rejected: the
      // overlay covers setMsg, and falling back silently looks like the file
      // was never read.
      if (doc && !ok) console.warn("SwanChat: chat.json rejected, using the built-in script");
      script = ok || sanitize(BUILT_IN) ||
          { cps: DEFAULT_CPS, lines: [{ who: "sys", text: "channel closed", pause_ms: 0 }] };
      pending = null;
      return script;
    });
    return pending;
  }

  // ------------------------------------------------------------------------
  // The window
  // ------------------------------------------------------------------------
  let root = null;      // the overlay, or null when closed
  let log = null;
  let cursor = null;
  let timer = null;
  // Every deferred paint carries the generation it was scheduled under.
  // clearTimeout on the handle we hold is not enough on its own: a callback
  // already in flight still runs, which is how flap.js used to land one stale
  // paint into its replacement.
  let gen = 0;
  let rearmAt = 0;

  const isOpen = () => root !== null;

  function scroll() {
    if (log) log.scrollTop = log.scrollHeight;
  }

  function addRow(who, text) {
    const row = document.createElement("div");
    row.className = "swan-chat-row swan-chat-" + who;
    const pre = document.createElement("span");
    pre.className = "swan-chat-pre";
    pre.textContent = has(PREFIX, who) ? PREFIX[who] : "";
    const t = document.createElement("span");
    t.className = "swan-chat-t";
    t.textContent = text || "";
    row.appendChild(pre);
    row.appendChild(t);
    log.appendChild(row);
    scroll();
    return t;
  }

  function build() {
    ensureStyles();
    root = document.createElement("div");
    root.className = "swan-chat";
    root.setAttribute("role", "dialog");
    root.setAttribute("aria-modal", "true");
    root.setAttribute("aria-label", "incoming channel");

    const bd = document.createElement("div");
    bd.className = "swan-chat-bd";

    const win = document.createElement("div");
    win.className = "swan-chat-win";

    const hd = document.createElement("div");
    hd.className = "swan-chat-hd";
    hd.textContent = "channel open";

    log = document.createElement("div");
    log.className = "swan-chat-log";
    // Polite, not assertive: a screen reader should read the line once it has
    // finished arriving, not re-announce it on every character.
    log.setAttribute("aria-live", "polite");

    const ft = document.createElement("div");
    ft.className = "swan-chat-ft";
    ft.textContent = "esc or clear to exit";

    win.appendChild(hd);
    win.appendChild(log);
    win.appendChild(ft);
    root.appendChild(bd);
    root.appendChild(win);
    document.body.appendChild(root);

    // The masher is a physical keyboard, so a phone can only reach this through
    // SwanChat.open() - and a phone has no ESC.  A tap anywhere closes it.
    root.addEventListener("pointerdown", () => close());

    // Capture, so the keys land here and NOT in the Numbers buffer behind the
    // overlay: terminal.js listens on window in the bubble phase, so stopping
    // propagation at window during capture leaves the entry line untouched.
    window.addEventListener("keydown", onKeyOpen, true);

    if (reduced()) {
      root.classList.add("up");
    } else {
      // Two frames: the element has to be laid out at opacity 0 before the
      // transition has anything to run from.
      requestAnimationFrame(() => {
        requestAnimationFrame(() => { if (root) root.classList.add("up"); });
      });
    }
  }

  function play(doc, g) {
    const lines = doc.lines;
    const period = Math.max(8, Math.round(1000 / doc.cps));

    if (reduced()) {
      // The end state, immediately.  No typing, no pauses, no cursor.
      for (let i = 0; i < lines.length; i++) addRow(lines[i].who, lines[i].text);
      return;
    }

    cursor = document.createElement("span");
    cursor.className = "swan-chat-cur";
    let line = 0;

    function nextLine() {
      if (g !== gen) return;
      if (line >= lines.length) {
        if (cursor && cursor.parentNode) cursor.parentNode.removeChild(cursor);
        return;                              // terminated: nothing left blinking
      }
      const ln = lines[line++];
      const span = addRow(ln.who, "");
      span.parentNode.appendChild(cursor);
      let n = 0;
      function chr() {
        if (g !== gen) return;
        n++;
        span.textContent = ln.text.slice(0, n);
        scroll();
        if (n < ln.text.length) {
          // Jitter: a metronome reads as a progress bar rather than as somebody
          // typing, and the typing is the one effect this whole thing rests on.
          timer = setTimeout(chr, period * (0.75 + Math.random() * 0.5));
        } else {
          timer = setTimeout(nextLine, ln.pause_ms);
        }
      }
      timer = setTimeout(chr, period);
    }

    nextLine();
  }

  function open() {
    if (root) return;
    gen++;
    const g = gen;
    build();
    click("key");
    loadScript().then((doc) => {
      if (g === gen && root) play(doc, g);
    });
  }

  function close() {
    if (!root) return;
    gen++;                                   // orphan every paint already in flight
    if (timer !== null) { clearTimeout(timer); timer = null; }
    window.removeEventListener("keydown", onKeyOpen, true);
    if (root.parentNode) root.parentNode.removeChild(root);
    root = null;
    log = null;
    cursor = null;
    // The fingers are still on the keys.  Drop what they typed and hold the
    // detector off for a moment, or the mash that closed it opens it again.
    buf.length = 0;
    rearmAt = Date.now() + REARM_MS;
    click("key");
  }

  function onKeyOpen(ev) {
    if (ev.metaKey || ev.ctrlKey || ev.altKey) return;  // leave F5 and ctrl-R alone
    ev.stopPropagation();
    if (ev.key === "Escape" || ev.key === " ") ev.preventDefault();
    // ESC is what terminal.js maps CLEAR to, so this IS the CLEAR key.
    if (ev.key === "Escape") close();
  }

  // ------------------------------------------------------------------------
  // The detector
  // ------------------------------------------------------------------------
  const buf = [];       // {k, t} for the keys still inside the window

  function onKeyMash(ev) {
    if (root) return;                                   // already open
    if (ev.metaKey || ev.ctrlKey || ev.altKey) return;
    if (ev.repeat) return;
    if (typeof ev.key !== "string" || ev.key.length !== 1) return;  // Enter, Tab, F5, arrows

    const now = Date.now();
    if (!eggArmed() || now < rearmAt) { buf.length = 0; return; }

    // A digit or a separator means this could be the Numbers, and the Numbers
    // must never open a chat window.  Not unlikely - impossible.
    if ((ev.key >= "0" && ev.key <= "9") || ev.key === " ") { buf.length = 0; return; }

    buf.push({ k: ev.key.toLowerCase(), t: now });
    while (buf.length && now - buf[0].t > MASH_W_MS) buf.shift();

    const seen = {};
    let distinct = 0;
    for (let i = 0; i < buf.length; i++) {
      if (!has(seen, buf[i].k)) { seen[buf[i].k] = 1; distinct++; }
    }
    if (distinct >= MASH_N) {
      buf.length = 0;
      open();
    }
  }

  // Arming the same element twice must not double-count, and it cannot: the
  // buffer counts DISTINCT keys, so a duplicated event for one physical press
  // adds nothing.  This list keeps the listener from stacking either.
  const armed = [];

  function armMasher(el) {
    const target = el || window;
    if (armed.indexOf(target) >= 0) return;
    armed.push(target);
    target.addEventListener("keydown", onKeyMash, false);
  }

  // The station screen stops keystrokes at document capture (it owns the
  // keyboard while it is up), so the masher armed on window never sees them.
  // It hands letters here instead, which keeps ONE detector rather than a
  // second copy that could drift.
  window.SwanChat = { armMasher, open, close, isOpen, feedKey: onKeyMash };

  // Armed on window at load, so the page needs nothing but the script tag.  It
  // is not "on": the detector reads prefs.egg at fire time, so the toggle takes
  // effect without a reload, and off means nothing happens at all.
  armMasher(window);
})();
