// Protocol mode (spec §15 phase 7): the purist Swan terminal.
//
// The friendly terminal shows you what is happening.  This one shows you what
// the Swan showed Desmond: a near-black screen, a blinking prompt, and a
// keyboard that does nothing at all until the timer has four minutes left.
// That inertness is the feature - it is what the show demonstrates, and a
// version that helpfully accepted the Numbers early would be a different prop.
//
// Screen-side only, with ONE exception, which is the same exception the whole
// phase has: EXECUTE goes through SwanTerm.send("countdown.execute", …), the
// identical §10.2a path the friendly terminal's own EXECUTE key uses.  Nothing
// else here sends anything, and nothing here touches the flaps.
//
// TWO CONTRACTS ARE IMPLEMENTED HERE RATHER THAN INVENTED:
//
//   1. The SYSTEM FAILURE flood is FIRMWARE_SPEC §7.3's cadence, verbatim: no
//      separator between repeats, no emitted line breaks, one fourteen-character
//      repeat every 100 ms, starting at zero and stopping when the phase leaves
//      zero/spin/reveal, with no clear and no cadence reset on a restart.  The
//      separate terminal prop prints the same string at the same rate; a third
//      cadence invented here would be exactly the divergence that contract
//      exists to prevent.
//
//   2. Every beat is timed off the DEADLINE plus the retained timing keys
//      (cfg.zero_hold_s, cfg.spin_s), and the landing comes off the firmware's
//      `reveal` EVENT.  Never off the arrival time of a message: publish skew on
//      swan/countdown was measured at up to ~0.7 s (BRINGUP §30), so arrival is
//      not a clock.  The event is used because the landing genuinely cannot be
//      computed - after the alarm spin the columns converge from an unknown
//      index, measured at 2.45-2.48 s with the canon reveal and as little as
//      0.15 s with blanks.
"use strict";

(function () {
  // FIRMWARE_SPEC §7.3, and a cross-repo constant: fourteen characters, six
  // then one space then seven, no trailing space.  Do not tidy this string.
  const SYSTEM_FAILURE = "SYSTEM FAILURE";
  const REPEAT_MS = 100;          // one repeat per tick -> 140 characters/second

  // The flood is unbounded in principle and a browser is not.  Keeping the last
  // ~24 k characters is many screens' worth at any size and costs nothing; the
  // cadence is unaffected, which is the part that is contractual.
  const FLOOD_MAX = 24000;

  const MAX_ENTRY = 24;

  let el = null;          // the overlay, created on first use
  let parts = null;       // {prompt, entry, cursor, flood, status}
  let entry = "";
  let flood = "";
  let floodTimer = null;
  let pendingExecute = false;
  let revealLanded = false;
  let askingLog = false;   // PRINT LOG? Y/N is up
  let word = "";           // letters typed at the idle prompt (CHESS, LOG)

  const T = () => window.SwanTerm;

  const on = () => !!(T() && T().prefs.protocol);

  // ---------------------------------------------------------------------
  // What the terminal is doing right now, derived from the deadline rather
  // than from whatever message last arrived.
  //
  //   idle     nothing armed.  The prompt is awake; this is where the eggs live.
  //   asleep   a countdown is running with more than seconds_live_s to go.
  //            KEYS DO NOTHING VISIBLE.  This is the show-accurate state and the
  //            reason this mode exists.
  //   live     inside the live window.  The prompt has woken with the 4-minute
  //            cue and the Numbers can be entered.
  //   failure  zero and after: the flood.
  // ---------------------------------------------------------------------
  function situation() {
    const t = T();
    if (!t) return "idle";
    const ph = t.phase();
    if (ph === "zero" || ph === "spin" || ph === "reveal") return "failure";
    const rem = t.remaining();
    if (rem === null || ph !== "running") return "idle";
    // The deadline, not the message: `remaining` is computed from the device's
    // own clock via the skew this page already tracks.
    if (rem <= 0) return "failure";
    return rem <= t.secondsLive() ? "live" : "asleep";
  }

  function build() {
    if (el) return;
    el = document.createElement("div");
    el.id = "protocol";
    el.className = "protocol";
    el.innerHTML =
      '<div class="pr-status" id="pr-status"></div>' +
      '<div class="pr-line"><span class="pr-caret">&gt;:</span>' +
      '<span class="pr-entry" id="pr-entry"></span>' +
      '<span class="pr-cursor" id="pr-cursor"></span></div>' +
      '<div class="pr-ask" id="pr-ask"></div>' +
      '<div class="pr-flood" id="pr-flood"></div>';
    document.body.appendChild(el);
    parts = {
      status: el.querySelector("#pr-status"),
      entry: el.querySelector("#pr-entry"),
      cursor: el.querySelector("#pr-cursor"),
      flood: el.querySelector("#pr-flood"),
      ask: el.querySelector("#pr-ask"),
    };
  }

  function destroy() {
    stopFlood();
    if (el && el.parentNode) el.parentNode.removeChild(el);
    el = null;
    parts = null;
    entry = "";
  }

  // ---------------------------------------------------------------------
  // The flood.  §7.3's cadence, and nothing else.
  // ---------------------------------------------------------------------
  function startFlood() {
    if (floodTimer !== null) return;   // already running: do NOT reset the cadence
    floodTimer = setInterval(() => {
      flood += SYSTEM_FAILURE;         // no separator, and no newline: §7.3
      if (flood.length > FLOOD_MAX) flood = flood.slice(flood.length - FLOOD_MAX);
      if (parts) {
        parts.flood.textContent = flood;
        // The paper always shows its own end.
        parts.flood.scrollTop = parts.flood.scrollHeight;
      }
    }, REPEAT_MS);
  }

  function stopFlood() {
    if (floodTimer === null) return;
    clearInterval(floodTimer);
    floodTimer = null;
  }

  // ---------------------------------------------------------------------
  // The failure beat, timed off the deadline and the retained timing keys.
  // ---------------------------------------------------------------------
  function beat() {
    const t = T();
    const s = t && t.state;
    if (!s) return "";
    const rem = t.remaining();
    if (rem === null) return "";
    const since = -rem;                       // seconds since the deadline
    const hold = (s.cfg && s.cfg.zero_hold_s) || 0;
    const spin = (s.cfg && s.cfg.spin_s) || 0;
    if (since < hold) return "SYSTEM FAILURE";
    if (since < hold + spin) return "SYSTEM FAILURE · DISCHARGE";
    // Past the computed beats the display is converging on the reveal, and only
    // the firmware knows when it arrives - so this last one waits for the event.
    return revealLanded ? "SYSTEM FAILURE · SEALED"
                        : "SYSTEM FAILURE · SEALING";
  }

  function renderAsk() {
    if (!parts) return;
    parts.ask.textContent = askingLog
        ? "PRINT LOG? Y/N"
        : (situation() === "idle" ? "TYPE LOG TO PRINT THE STATION RECORD" : "");
  }

  let lastSit = null;

  function render() {
    if (!el) return;
    const sit = situation();
    if (sit !== lastSit) {
      // Anything half-typed belongs to the situation it was typed in.  Without
      // this, digits left at the idle prompt are still sitting there when the
      // countdown reaches the live window, and the next EXECUTE sends them
      // joined onto whatever the operator types next.
      entry = "";
      askingLog = false;
      word = "";
      lastSit = sit;
    }
    el.dataset.sit = sit;

    if (sit === "failure") {
      startFlood();
      parts.status.textContent = beat();
      parts.entry.textContent = "";
      parts.ask.textContent = "";
      askingLog = false;
      return;
    }
    stopFlood();
    renderAsk();

    const t = T();
    if (sit === "asleep") {
      // Nothing. Not a hint, not a countdown, not a "wait" - the screen the show
      // put on the wall gave you a blinking cursor and no acknowledgement at
      // all, and the whole point of this mode is to reproduce that.
      parts.status.textContent = "";
      parts.entry.textContent = "";
      return;
    }
    if (sit === "live") {
      // The spec 7.3 contract, through the host so there is one implementation:
      // ceil(remaining / step) * step.  A protocol screen that disagreed with
      // the flaps beside it by a whole second would be the exact defect the
      // contract exists to prevent.
      const shown = t.shownS(Math.max(0, t.remaining()));
      const m = Math.floor(shown / 60);
      const ss = String(shown % 60).padStart(2, "0");
      parts.status.textContent = m + ":" + ss;
    } else {
      parts.status.textContent = "";
    }
    parts.entry.textContent = entry;
  }

  // ---------------------------------------------------------------------
  // Input.  `asleep` swallows everything, which is the feature.
  // ---------------------------------------------------------------------
  function accepts() {
    const sit = situation();
    return sit === "live" || sit === "idle";
  }

  function onKey(e) {
    if (!on() || !el) return false;
    // Let the eggs have the idle prompt to themselves; they arm their own
    // listeners and decide for themselves whether they are enabled.
    const k = e.key;

    if (k === "Escape") {
      entry = "";
      render();
      return true;
    }
    if (!accepts()) {
      // Swallowed on purpose.  Preventing the default too, so a stray key does
      // not scroll the page behind a screen that is pretending to ignore it.
      e.preventDefault();
      return true;
    }
    // ---- the idle prompt's own affordances (phase 7) -------------------
    // All of them are screen-side and all are behind PREFS.egg except the log,
    // which is the display's own history and not an easter egg.
    if (situation() === "idle") {
      if (askingLog) {
        if (k === "y" || k === "Y") {
          askingLog = false;
          renderAsk();
          if (window.SwanPearl) window.SwanPearl.open();
          T().clickSound("exec");
          e.preventDefault();
          return true;
        }
        if (k === "n" || k === "N" || k === "Escape") {
          askingLog = false;
          renderAsk();
          e.preventDefault();
          return true;
        }
      }
      // A word typed at the prompt, rather than the Numbers.
      if (/^[a-zA-Z]$/.test(k)) {
        word = (word + k.toUpperCase()).slice(-8);
        if (word.endsWith("CHESS")) {
          word = "";
          if (window.SwanChess) window.SwanChess.open();
          e.preventDefault();
          return true;
        }
        if (word.endsWith("LOG") || word.endsWith("PRINT")) {
          word = "";
          askingLog = true;
          renderAsk();
          e.preventDefault();
          return true;
        }
        T().clickSound("key");
        e.preventDefault();
        return true;      // letters never enter the Numbers
      }
    }

    if (k >= "0" && k <= "9") {
      if (entry.length < MAX_ENTRY) entry += k;
    } else if (k === " ") {
      if (entry.length < MAX_ENTRY && entry.slice(-1) !== " ") entry += " ";
      e.preventDefault();
    } else if (k === "Backspace") {
      entry = entry.slice(0, -1);
      e.preventDefault();
    } else if (k === "Enter") {
      execute();
      e.preventDefault();
    } else {
      return false;      // not ours; the eggs may want it
    }
    T().clickSound("key");
    render();
    return true;
  }

  function execute() {
    const t = T();
    if (!t.timeValid()) {
      flash("CLOCK NOT SYNCED");
      return;
    }
    const numbers = entry.trim().replace(/\s+/g, " ");
    if (!numbers) return;
    // THE ONE COMMAND THIS ENTIRE PHASE SENDS.
    t.send("countdown.execute", numbers);
    pendingExecute = true;
    t.clickSound("exec");
  }

  function flash(text) {
    if (!parts) return;
    parts.status.textContent = text;
    clearTimeout(flash.t);
    flash.t = setTimeout(render, 2200);
  }

  // ---------------------------------------------------------------------
  function apply() {
    const want = on();
    document.documentElement.classList.toggle("protocol-on", want);
    if (want) {
      build();
      render();
    } else {
      destroy();
    }
  }

  function init() {
    const t = T();
    if (!t) return;

    t.on("state", () => { if (on()) render(); })
     .on("phase", (ph) => {
       if (ph === "zero") revealLanded = false;
       // Leaving the failure states is the only thing that stops the flood
       // (§7.3).  The screen is NOT cleared: `flood` survives, so a second run
       // continues the same paper rather than starting a fresh one.
       if (on()) render();
     })
     .on("reveal", () => {
       revealLanded = true;
       if (on()) render();
     })
     .on("cue", (e) => {
       // The prompt wakes WITH the alarm, because they are the same moment: the
       // 4-minute cue and countdown.seconds_live_s are the same 240 seconds.
       if (on() && e && e.name === "warn_4min") render();
     });

    document.addEventListener("keydown", (e) => {
      if (!on()) return;
      const tag = (e.target && e.target.tagName) || "";
      if (tag === "INPUT" || tag === "TEXTAREA") return;
      onKey(e);
    }, true);

    // A result for our own EXECUTE, and only ours - `pendingExecute` is what
    // makes it ours.  This used to reach for a `SwanBusResultHook` global that
    // never existed, so the purist terminal silently gave no feedback at all:
    // press EXECUTE with the wrong Numbers and the screen just carried on
    // counting.  Found by driving it on the board rather than by reading it.
    t.on("result", onResult);

    setInterval(() => { if (on()) render(); }, 250);
    apply();
  }

  function onResult(res) {
    if (!pendingExecute) return;
    pendingExecute = false;
    if (!res) return;
    if (res.ok) {
      entry = "";
      flash("ACCEPTED");
    } else {
      flash(res.err === "rejected" ? "INCORRECT" : String(res.err || "REJECTED").toUpperCase());
    }
  }

  window.SwanProtocol = {
    apply,
    isOn: on,
    situation,
    // For the eggs: they may only open from the idle prompt.
    idle: () => situation() === "idle",
    // Exposed for the JS suite: the flood is a contract, so it is testable
    // without a browser.
    _flood: () => flood,
    _tickFlood: () => {
      flood += SYSTEM_FAILURE;
      if (flood.length > FLOOD_MAX) flood = flood.slice(flood.length - FLOOD_MAX);
      return flood;
    },
    _resetForTest: () => { flood = ""; entry = ""; },
    SYSTEM_FAILURE,
    REPEAT_MS,
  };

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
  } else {
    init();
  }
})();
