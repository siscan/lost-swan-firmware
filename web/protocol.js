// The station screen (spec §15 phase 7, restructured 2026-08-25).
//
// One purist terminal, three station personas. SWAN, PEARL and FLAME share the
// renderer, the CRT, the keyboard, the echo and the teletype - and share them
// freely - but never at each other's expense: no station's commands leak into
// another, and each is complete on its own.
//
//   SWAN   the Numbers and EXECUTE, the live window, the SYSTEM FAILURE
//          choreography, and the chat easter egg. No LOG, no CHESS.
//   PEARL  boots to PRINT LOG? Y/N and prints the device's own journal.
//   FLAME  boots to the chess offer, and Chang's menu on a win.
//
// PEARL AND FLAME ARE INDIFFERENT TO COUNTDOWN STATE. Only Swan reacts to the
// deadline, because only Swan is the countdown's station.
//
// FOUR RULES THIS FILE EXISTS TO KEEP:
//
//  1. Content and presentation are orthogonal. Station and protocol are
//     content; CRT, key click, mirror and fullscreen are presentation. Every
//     combination composes, no toggle writes another toggle, each persists on
//     its own. The screen renders INSIDE the CRT filter (`.crt-layer`), which
//     is what stopped protocol mode punching through it - the purist mode was
//     the one losing the phosphor, which is backwards.
//  2. No persisted mode without an always-available escape, mouse and keyboard
//     both. Pointer movement, a click on dead space or a tap reveals the strip;
//     ESC leaves protocol mode; PANEL at any idle prompt goes to the control
//     panel.
//  3. Accepted input echoes. Everywhere input is taken it appears at `>:` with
//     working DEL and CLEAR. Inert-with-no-echo applies to exactly one state:
//     a running countdown above the 4:00 mark, which is what the show does.
//  4. Output teletypes at one documented cadence. Input echoes instantly, and
//     any key completes an in-progress print so it never slows anybody down.
//     The SYSTEM FAILURE flood keeps its separate contractual rate.
"use strict";

(function () {
  // FIRMWARE_SPEC §7.3, a cross-repo constant: fourteen characters, six then
  // one space then seven, no trailing space. Do not tidy this string.
  const SYSTEM_FAILURE = "SYSTEM FAILURE";
  const FLOOD_REPEAT_MS = 100;    // one repeat per tick -> 140 characters/second

  // RULE 4's ONE CADENCE, for every teletyped line on every station.
  // 45 characters a second: fast enough that a hint is never a wait, slow
  // enough to read as a machine printing rather than as text appearing. It is
  // deliberately NOT the flood's rate - that one is a contract shared with the
  // terminal prop and is left exactly alone.
  const TELETYPE_CPS = 45;
  const TELETYPE_MS = Math.round(1000 / TELETYPE_CPS);   // 22 ms per character

  const FLOOD_MAX = 24000;
  const MAX_ENTRY = 24;

  // ---------------------------------------------------------------------
  // The stations.
  //
  // STATION NUMBERS, verified by Nico against Lostpedia's station list
  // 2026-08-25. The full canon is Hydra 1, Arrow 2, Swan 3, Flame 4, Pearl 5,
  // Orchid 6 - so the Flame is 4 and the Pearl is 5, and NOT the other way
  // round, which is the mistake a guess would most likely have made. They were
  // printed without numbers until somebody checked, because a wrong number on
  // a prop is worse than no number. test_toggles.js pins all three headers
  // against that list.
  // ---------------------------------------------------------------------
  const STATIONS = {
    swan: {
      label: "SWAN",
      header: "STATION 3 · THE SWAN",
      hint: "ENTER THE NUMBERS, THEN EXECUTE.",
    },
    pearl: {
      label: "PEARL",
      header: "STATION 5 · THE PEARL",
      hint: "TYPE LOG TO PRINT THE STATION RECORD",
    },
    flame: {
      label: "FLAME",
      header: "STATION 4 · THE FLAME",
      hint: "TYPE CHESS TO PLAY",
    },
  };
  const STATION_NAMES = ["swan", "pearl", "flame"];

  let el = null;
  let parts = null;
  let entry = "";
  let flood = "";
  let floodTimer = null;
  let pendingExecute = false;
  let revealLanded = false;
  let asking = null;        // "log" | "chess" | null - a pending Y/N
  let word = "";
  let lastSit = null;
  let booted = false;       // this station has printed its opening line

  const T = () => window.SwanTerm;
  const on = () => !!(T() && T().prefs.protocol);
  const station = () => {
    const t = T();
    const s = t && t.station ? t.station() : "swan";
    return STATIONS[s] ? s : "swan";
  };

  // ---------------------------------------------------------------------
  // RULE 4: the teletype.
  // ---------------------------------------------------------------------
  let ttQueue = [];
  let ttTimer = null;
  let ttLine = "";
  let ttAt = 0;

  function say(text) {
    ttQueue.push(String(text));
    pumpTeletype();
  }

  function pumpTeletype() {
    if (ttTimer !== null || !parts) return;
    if (!ttLine) {
      if (!ttQueue.length) return;
      ttLine = ttQueue.shift();
      ttAt = 0;
      const d = document.createElement("div");
      d.className = "said";
      parts.out.appendChild(d);
    }
    ttTimer = setInterval(() => {
      const node = parts && parts.out.lastChild;
      if (!node) { finishTeletype(); return; }
      ttAt++;
      node.textContent = ttLine.slice(0, ttAt);
      if (ttAt >= ttLine.length) {
        clearInterval(ttTimer);
        ttTimer = null;
        ttLine = "";
        trimOut();
        if (ttQueue.length) pumpTeletype();
      }
    }, TELETYPE_MS);
  }

  // RULE 4: any key completes the print at once. A flourish that makes somebody
  // wait has stopped being one.
  function finishTeletype() {
    if (ttTimer !== null) { clearInterval(ttTimer); ttTimer = null; }
    if (!parts) { ttLine = ""; ttQueue = []; return; }
    if (ttLine) {
      const node = parts.out.lastChild;
      if (node) node.textContent = ttLine;
      ttLine = "";
    }
    while (ttQueue.length) {
      const d = document.createElement("div");
      d.className = "said";
      d.textContent = ttQueue.shift();
      parts.out.appendChild(d);
    }
    trimOut();
  }

  function trimOut() {
    if (!parts) return;
    while (parts.out.childNodes.length > 40) parts.out.removeChild(parts.out.firstChild);
    parts.out.scrollTop = parts.out.scrollHeight;
  }

  function clearOut() {
    finishTeletype();
    if (parts) parts.out.textContent = "";
    ttQueue = [];
  }

  // ---------------------------------------------------------------------
  // What the SWAN screen is doing. Pearl and Flame never consult this.
  // ---------------------------------------------------------------------
  function situation() {
    const t = T();
    if (!t) return "idle";
    if (station() !== "swan") return "idle";     // other stations have no phases
    const ph = t.phase();
    if (ph === "zero" || ph === "spin" || ph === "reveal") return "failure";
    const rem = t.remaining();
    if (rem === null || ph !== "running") return "idle";
    if (rem <= 0) return "failure";
    return rem <= t.secondsLive() ? "live" : "asleep";
  }

  // RULE 3: input is accepted, and therefore echoed, everywhere except one
  // state - a countdown running above the 4:00 mark, on Swan. That inertness is
  // the show's behaviour and the reason this mode exists; everywhere else,
  // typing blind is just a bug.
  function accepts() { return situation() !== "asleep"; }

  // ---------------------------------------------------------------------
  function build() {
    if (el) return;
    const host = document.getElementById("screen") || document.body;
    el = document.createElement("div");
    el.id = "protocol";
    // `crt-layer` is rule 1's structural half: the CRT filter styles that class,
    // so the phosphor wraps the station screen instead of being punched through.
    el.className = "protocol crt-layer";
    el.innerHTML =
      '<div class="pr-head" id="pr-head"></div>' +
      '<div class="pr-status" id="pr-status"></div>' +
      '<div class="pr-out" id="pr-out"></div>' +
      '<div class="pr-line"><span class="pr-caret">&gt;:</span>' +
      '<span class="pr-entry" id="pr-entry"></span>' +
      '<span class="pr-cursor" id="pr-cursor"></span></div>' +
      '<div class="pr-flood" id="pr-flood"></div>';
    host.appendChild(el);
    parts = {
      head: el.querySelector("#pr-head"),
      status: el.querySelector("#pr-status"),
      out: el.querySelector("#pr-out"),
      entry: el.querySelector("#pr-entry"),
      flood: el.querySelector("#pr-flood"),
    };
    booted = false;
  }

  function destroy() {
    stopFlood();
    finishTeletype();
    if (el && el.parentNode) el.parentNode.removeChild(el);
    el = null;
    parts = null;
    entry = "";
    asking = null;
    booted = false;
  }

  // ---------------------------------------------------------------------
  // The flood. §7.3's cadence, and nothing else. Swan only.
  // ---------------------------------------------------------------------
  function startFlood() {
    if (floodTimer !== null) return;   // running: do NOT reset the cadence
    floodTimer = setInterval(() => {
      flood += SYSTEM_FAILURE;         // no separator, and no newline: §7.3
      if (flood.length > FLOOD_MAX) flood = flood.slice(flood.length - FLOOD_MAX);
      if (parts) {
        parts.flood.textContent = flood;
        parts.flood.scrollTop = parts.flood.scrollHeight;
      }
    }, FLOOD_REPEAT_MS);
  }

  function stopFlood() {
    if (floodTimer === null) return;
    clearInterval(floodTimer);
    floodTimer = null;
  }

  function beat() {
    const t = T();
    const s = t && t.state;
    if (!s) return "";
    const rem = t.remaining();
    if (rem === null) return "";
    const since = -rem;
    const hold = (s.cfg && s.cfg.zero_hold_s) || 0;
    const spin = (s.cfg && s.cfg.spin_s) || 0;
    if (since < hold) return SYSTEM_FAILURE;
    if (since < hold + spin) return SYSTEM_FAILURE + " · DISCHARGE";
    return revealLanded ? SYSTEM_FAILURE + " · SEALED"
                        : SYSTEM_FAILURE + " · SEALING";
  }

  // ---------------------------------------------------------------------
  // Each station's opening line, printed once when the screen appears.
  // ---------------------------------------------------------------------
  function bootStation() {
    if (booted) return;
    booted = true;
    clearOut();
    asking = null;
    const st = station();
    if (st === "pearl") {
      asking = "log";
      say("PRINT LOG? Y/N");
    } else if (st === "flame") {
      asking = "chess";
      say("CHESS? Y/N");
    } else if (situation() === "idle") {
      say(STATIONS.swan.hint);
    }
  }

  function render() {
    if (!el) return;
    const st = station();
    const sit = situation();
    if (sit !== lastSit) {
      entry = "";            // half-typed input belongs to the state it was typed in
      asking = null;
      word = "";
      lastSit = sit;
    }
    el.dataset.sit = sit;
    el.dataset.station = st;
    parts.head.textContent = STATIONS[st].header;

    if (sit === "failure") {
      startFlood();
      parts.status.textContent = beat();
      parts.entry.textContent = "";
      return;
    }
    stopFlood();
    bootStation();

    const t = T();
    if (sit === "asleep") {
      // Nothing at all. Not a hint, not a countdown, not a "wait" - the screen
      // the show put on the wall gave you a cursor and no acknowledgement, and
      // reproducing that is the whole point of this mode.
      parts.status.textContent = "";
      parts.entry.textContent = "";
      return;
    }
    if (sit === "live") {
      const shown = t.shownS(Math.max(0, t.remaining()));
      parts.status.textContent =
          Math.floor(shown / 60) + ":" + String(shown % 60).padStart(2, "0");
    } else {
      parts.status.textContent = "";
    }
    parts.entry.textContent = entry;   // RULE 3: what you typed, where you typed it
  }

  // ---------------------------------------------------------------------
  // Input.
  // ---------------------------------------------------------------------
  function onKey(e) {
    if (!on() || !el) return;
    const k = e.key;

    // ESC always leaves protocol mode (rule 2), before anything else can
    // swallow it.
    if (k === "Escape") {
      e.preventDefault();
      e.stopPropagation();
      const t = T();
      t.prefs.protocol = false;
      t.savePref("protocol");
      t.applyPrefs();
      return;
    }

    // RULE 4: any key finishes an in-progress print immediately.
    if (ttTimer !== null || ttQueue.length) finishTeletype();

    // WE OWN THE KEYBOARD WHILE WE ARE UP. terminal.js binds its own window
    // handler, and without stopping propagation every keystroke reached BOTH -
    // which is why one press of EXECUTE started two countdowns (the journal
    // shows seq 1 and seq 2 in the same second), and why typing CHESS raised
    // the friendly terminal's CANCEL confirm on the C.
    e.stopPropagation();

    if (!accepts()) { e.preventDefault(); return; }

    const st = station();

    // A pending Y/N owns the next key.
    if (asking) {
      if (k === "y" || k === "Y") {
        const what = asking;
        asking = null;
        e.preventDefault();
        T().clickSound("exec");
        if (what === "log" && window.SwanPearl) window.SwanPearl.open();
        if (what === "chess" && window.SwanChess) window.SwanChess.open();
        render();
        return;
      }
      if (k === "n" || k === "N") {
        asking = null;
        e.preventDefault();
        say(STATIONS[st].hint);
        render();
        return;
      }
    }

    if (k === "Backspace") {
      entry = entry.slice(0, -1);
      word = word.slice(0, -1);
      e.preventDefault();
      T().clickSound("key");
      render();
      return;
    }

    if (k >= "0" && k <= "9") {
      // Digits are Swan's - the Numbers. The other stations have nothing to do
      // with them and must not pretend to accept them.
      if (st === "swan" && entry.length < MAX_ENTRY) entry += k;
      e.preventDefault();
      T().clickSound("key");
      render();
      return;
    }

    if (k === " ") {
      if (st === "swan" && entry.length < MAX_ENTRY && entry.slice(-1) !== " ") entry += " ";
      e.preventDefault();
      T().clickSound("key");
      render();
      return;
    }

    if (k === "Enter") {
      e.preventDefault();
      runWord(false);
      return;
    }

    if (/^[a-zA-Z]$/.test(k)) {
      // RULE 3 again: letters ECHO. They used to be swallowed, so LOG was typed
      // blind - the machine took the command with nothing on screen saying so.
      word = (word + k.toUpperCase()).slice(-12);
      if (entry.length < MAX_ENTRY) entry += k.toUpperCase();
      e.preventDefault();
      T().clickSound("key");
      render();
      runWord(true);
      // The chat egg is SWAN'S, and it lives on a mash detector we have just
      // stopped from reaching window.  Hand it the key rather than keeping a
      // second copy of the detector here.
      if (st === "swan" && window.SwanChat && window.SwanChat.feedKey) {
        window.SwanChat.feedKey(e);
      }
    }
  }

  // Commands at an idle prompt. `implicit` means it matched as a suffix while
  // typing rather than being submitted with Enter.
  function runWord(implicit) {
    const w = word;
    const st = station();
    const hit = (name) => w.endsWith(name);

    // EVERY STATION: the escape hatches (rule 2) and the station switch.
    if (hit("PANEL")) { clearEntry(); window.location.href = "index.html"; return; }
    for (const name of STATION_NAMES) {
      if (hit(name.toUpperCase())) {
        clearEntry();
        if (name !== st) T().setStation(name);
        return;
      }
    }

    if (st === "swan") {
      if (hit("LOGO")) {
        clearEntry();
        if (window.SwanBoot) window.SwanBoot.play({ skipable: true });
        return;
      }
      // LOG and CHESS are deliberately NOT here. They belong to Pearl and
      // Flame; a station's commands must not leak into another's prompt.
      if (!implicit) execute();
      return;
    }

    if (st === "pearl") {
      if (hit("LOG") || hit("PRINT")) {
        clearEntry();
        asking = "log";
        say("PRINT LOG? Y/N");
        render();
      }
      return;
    }

    if (st === "flame") {
      if (hit("CHESS")) {
        clearEntry();
        if (window.SwanChess) window.SwanChess.open();
      }
    }
  }

  function clearEntry() {
    entry = "";
    word = "";
    render();
  }

  function execute() {
    const t = T();
    if (!t.timeValid()) { say("CLOCK NOT SYNCED - THE DEADLINE CANNOT BE SET YET"); return; }
    const numbers = entry.trim().replace(/\s+/g, " ");
    if (!numbers) { say(STATIONS.swan.hint); return; }
    // THE ONE COMMAND THIS ENTIRE PACK SENDS.
    t.send("countdown.execute", numbers);
    pendingExecute = true;
    t.clickSound("exec");
  }

  function onResult(res) {
    if (!pendingExecute) return;
    pendingExecute = false;
    if (!res) return;
    if (res.ok) {
      clearEntry();
      say("ACCEPTED");
    } else {
      say(res.err === "rejected" ? "INCORRECT - ENTER THE NUMBERS"
                                 : String(res.err || "REJECTED").toUpperCase());
    }
  }

  // ---------------------------------------------------------------------
  function apply() {
    const want = on();
    document.documentElement.classList.toggle("protocol-on", want);
    if (want) { build(); render(); } else { destroy(); }
  }

  function onStation() {
    // Switching station resets the screen and touches NO presentation toggle.
    booted = false;
    asking = null;
    entry = "";
    word = "";
    clearOut();
    if (window.SwanPearl && window.SwanPearl.isOpen()) window.SwanPearl.close();
    if (window.SwanChess && window.SwanChess.isOpen()) window.SwanChess.close();
    render();
  }

  function init() {
    const t = T();
    if (!t) return;

    t.on("state", () => { if (on()) render(); })
     .on("phase", (ph) => { if (ph === "zero") revealLanded = false; if (on()) render(); })
     .on("reveal", () => { revealLanded = true; if (on()) render(); })
     .on("result", onResult)
     .on("station", onStation)
     .on("prefs", apply);

    // Capture phase, so the station screen sees keys before the friendly
    // terminal's window handler - and stops them there.
    document.addEventListener("keydown", (e) => {
      if (!on()) return;
      const tag = (e.target && e.target.tagName) || "";
      if (tag === "INPUT" || tag === "TEXTAREA" || tag === "BUTTON" || tag === "A") return;
      if (e.metaKey || e.ctrlKey || e.altKey) return;
      // A station feature that has taken the screen owns its own keys.
      if (window.SwanChess && window.SwanChess.isOpen && window.SwanChess.isOpen()) return;
      if (window.SwanChat && window.SwanChat.isOpen && window.SwanChat.isOpen()) return;
      if (window.SwanPearl && window.SwanPearl.isOpen && window.SwanPearl.isOpen()) return;
      onKey(e);
    }, true);

    setInterval(() => { if (on()) render(); }, 250);
    apply();
  }

  window.SwanProtocol = {
    apply,
    isOn: on,
    situation,
    idle: () => situation() === "idle",
    station,
    say,
    _flood: () => flood,
    _entry: () => entry,
    _finishTeletype: finishTeletype,
    _resetForTest: () => { flood = ""; entry = ""; word = ""; asking = null; },
    SYSTEM_FAILURE,
    FLOOD_REPEAT_MS,
    TELETYPE_CPS,
    STATIONS,
  };

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
  } else {
    init();
  }
})();
