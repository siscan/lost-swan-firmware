// The acceptance checklist page (web/qa.html).
//
// A SEPARATE FILE on purpose: tools/jscheck.py and CI's `node --check` both
// scan web/*.js and neither can see inside an inline <script>.  A syntax error
// in this repo does not show up as an error - it shows up as a blank page on a
// perfectly healthy board, and that has happened three times.  Inline script in
// a page is exactly how it would happen a fourth.
//
// The rows are written from docs/FIRMWARE_SPEC.md - what the firmware is
// SUPPOSED to do, in plain words - and deliberately not from reading the
// implementation, because a checklist derived from the code can only ever
// confirm that the code does what the code does.
"use strict";

// ---------------------------------------------------------------------------
// The checklist.  Sourced from the spec, deliberately: `expect` is what the
// document promises in plain words, not a description of the implementation.
// ---------------------------------------------------------------------------
var SECTIONS = [

{ id: "known", title: "KNOWN FAILURES GOING IN",
  note: "Already reported, already reproduced. Confirm they still look like this, and note anything different. These are not fresh findings.",
  rows: [
  { id: "K-1", title: "Swan boot mark draws as filled blobs, not as inked strokes",
    known: true,
    expect: "The boot animation is supposed to DRAW the swan: the art supplies centreline spines with a width per vertex, each segment is stroked and dash-drawn so the line grows, and only then does the spine group crossfade into the filled silhouette. What actually happens is that filled blobs appear directly - the stroke-on stage is not visible.",
    steps: ["Load /terminal.html with the station set to SWAN.",
            "Watch the swan inside the octagon as the mark builds.",
            "Expected-per-spec: a line drawing that grows, then fills.",
            "Observed: shapes appear already filled."],
    ref: "spec §17 2026-08-25 (the mark is reference art); web/bootanim_logo.js" } ] },

{ id: "r1", title: "RULE 1 — CONTENT AND PRESENTATION ARE ORTHOGONAL",
  note: "Station and content mode say WHAT is on screen; CRT, key click, mirror and fullscreen say how it looks. Every combination composes, no toggle may write another toggle's state, each persists on its own.",
  rows: [
  { id: "R1-1", title: "The CRT survives the station screen",
    expect: "With CRT on, turning PROTOCOL on keeps the phosphor glow, the scanlines and the glass. The station screen renders INSIDE the CRT effect, not over the top of it.",
    steps: ["On /terminal.html turn CRT on. Note the look.",
            "Turn PROTOCOL on.",
            "The near-black station screen should still be visibly phosphor-glowing and scanlined."],
    ref: "spec §10.2b rule 1" },
  { id: "R1-2", title: "No toggle changes another toggle",
    expect: "Pressing any one of CRT, KEY CLICK, MIRROR, PROTOCOL or CHAT changes only itself. Nothing else on the strip lights up or goes dark as a side effect.",
    steps: ["Note which strip buttons are lit.",
            "Press one toggle. Check every other button is unchanged.",
            "Repeat for each of the five."],
    ref: "spec §10.2b rule 1" },
  { id: "R1-3", title: "Choosing a station changes no presentation setting",
    expect: "Pressing SWAN, PEARL or FLAME changes the station and nothing else. CRT, KEY CLICK, MIRROR and FULLSCREEN are exactly as they were.",
    steps: ["Set a distinctive combination, e.g. CRT on and MIRROR off.",
            "Press PEARL, then FLAME, then SWAN.",
            "Confirm CRT is still on and MIRROR still off."],
    ref: "spec §10.2b rule 1" },
  { id: "R1-4", title: "Every setting survives a reload",
    expect: "Whatever combination is set, reloading the page brings it all back - including which station was selected.",
    steps: ["Set an unusual combination: CRT on, KEY CLICK off, MIRROR on, PROTOCOL on, station FLAME.",
            "Reload the page.",
            "Every one of those should come back as set."],
    ref: "spec §10.2b rule 1" },
  { id: "R1-5", title: "A second browser has its own settings",
    expect: "Presentation preferences are per browser and never stored on the device. A phone and a laptop looking at the same display do not fight over each other's scanlines.",
    steps: ["Set CRT on here.",
            "Open /terminal.html on a phone or a different browser.",
            "The second browser should be at its own defaults, and changing it must not change this one."],
    ref: "spec §10.2 (preferences live in localStorage, never in NVS)" },
  { id: "R1-6", title: "Fullscreen is an action, not a saved setting",
    expect: "FULLSCREEN takes effect immediately but is deliberately NOT remembered across a reload - browsers only allow fullscreen from a deliberate press, so a saved setting could not be honoured honestly.",
    steps: ["Press FULLSCREEN.", "Reload the page.", "The page should come back not fullscreen, and that is correct."],
    ref: "spec §10.2b (fullscreen does not persist, and cannot)" } ] },

{ id: "r2", title: "RULE 2 — THERE IS ALWAYS A WAY OUT",
  note: "No persisted mode without an escape available to mouse AND keyboard. A persisted mode with no way out is a trap, and this is a web page.",
  rows: [
  { id: "R2-1", title: "The strip hides itself, and comes back on a mouse move",
    expect: "In the station screen the toggle strip fades away after about five seconds untouched. Moving the mouse anywhere brings it straight back.",
    steps: ["Turn PROTOCOL on.", "Leave the mouse and keyboard alone for five seconds - the strip should fade out.",
            "Move the mouse without clicking. The strip should reappear."],
    ref: "spec §10.2b rule 2, STRIP_MS = 5000" },
  { id: "R2-2", title: "A click on empty space brings the strip back",
    expect: "Clicking dead space - not a button - reveals the strip. A mouse-only user is never stuck.",
    steps: ["Let the strip fade.", "Click on an empty part of the screen.", "The strip should reappear."],
    ref: "spec §10.2b rule 2" },
  { id: "R2-3", title: "A tap brings the strip back (touch device)",
    expect: "On a phone or tablet, tapping anywhere reveals the strip. Mark N/A if you have no touch device to hand.",
    steps: ["On a touch device open /terminal.html and turn PROTOCOL on.",
            "Let the strip fade, then tap the screen.", "The strip should reappear."],
    ref: "spec §10.2b rule 2" },
  { id: "R2-4", title: "ESC leaves the station screen",
    expect: "Pressing Escape returns to the ordinary terminal, from any station and at any time.",
    steps: ["Turn PROTOCOL on.", "Press Escape.", "You should be back on the friendly terminal.",
            "Repeat on PEARL and on FLAME."],
    ref: "spec §10.2b rule 2" },
  { id: "R2-5", title: "Typing PANEL goes to the control panel",
    expect: "At any idle prompt, typing PANEL navigates to the main control panel. It works on all three stations.",
    steps: ["On the station screen at an idle prompt type PANEL.",
            "The control panel should load.", "Go back and repeat from PEARL and FLAME."],
    ref: "spec §10.2b rule 2" },
  { id: "R2-6", title: "The strip always carries the same escapes",
    expect: "Whenever the strip is visible it carries CONTROL PANEL, REPLAY LOGO and the three station buttons - never a reduced set.",
    steps: ["Reveal the strip on each station in turn.",
            "Confirm CONTROL PANEL, REPLAY LOGO, SWAN, PEARL and FLAME are all present each time."],
    ref: "spec §10.2b rule 2" },
  { id: "R2-7", title: "ESC still works when the screen is deliberately inert",
    expect: "During a countdown above the four-minute mark the Swan screen ignores typing on purpose - but Escape must still get you out. The one inert state is not allowed to become a trap.",
    steps: ["Start a countdown (EXECUTE with the Numbers) and turn PROTOCOL on with station SWAN.",
            "While more than 4:00 remains, type some letters - nothing should appear.",
            "Press Escape. You should leave the station screen."],
    ref: "spec §10.2b rules 2 and 3" } ] },

{ id: "r3", title: "RULE 3 — ACCEPTED INPUT ECHOES",
  note: "Wherever input is taken it appears at the >: caret. Typing blind is a bug everywhere except one deliberate state.",
  rows: [
  { id: "R3-1", title: "The Numbers echo as you type them",
    expect: "On the Swan station with no countdown running, typing digits and spaces shows them at the >: caret as you go.",
    steps: ["PROTOCOL on, station SWAN, no countdown running.",
            "Type 4 8 15 16 23 42.", "Each character should appear at the caret."],
    ref: "spec §10.2b rule 3" },
  { id: "R3-2", title: "Backspace deletes one character",
    expect: "Backspace removes the last character typed, one at a time.",
    steps: ["Type a few digits.", "Press Backspace twice.", "Two characters should disappear."],
    ref: "spec §10.2b rule 3 (working DEL and CLEAR)" },
  { id: "R3-3", title: "Letters echo too",
    expect: "Typing letters shows them at the caret, on every station - even letters that are not a command. You should never be typing invisibly.",
    steps: ["On PEARL type CHESS (which is the Flame's command, not the Pearl's).",
            "The letters should still appear at the caret, and nothing else should happen."],
    ref: "spec §10.2b rule 3" },
  { id: "R3-4", title: "The one inert state: a countdown above 4:00 on Swan",
    expect: "While a countdown is running and more than countdown.seconds_live_s (4:00 by default) remains, the Swan screen shows nothing and accepts nothing. That silence is the show and is deliberate.",
    steps: ["Start a countdown, PROTOCOL on, station SWAN.",
            "With more than 4:00 remaining, type anything.",
            "Nothing should appear - no caret text, no readout, no hint."],
    ref: "spec §10.2b rule 3" },
  { id: "R3-5", title: "Inside the last four minutes, typing works again",
    expect: "Once the countdown drops under four minutes the screen wakes: the remaining time is shown and typing echoes again.",
    steps: ["Let a countdown reach under 4:00 (or set a deadline about three minutes out).",
            "The readout should appear.", "Type digits - they should echo."],
    ref: "spec §10.2b rule 3, §7.3" },
  { id: "R3-6", title: "A Y/N question takes the key without echoing it",
    expect: "This is the one deliberate exception. PEARL and FLAME open with a Y/N question; pressing Y or N acts immediately rather than printing the letter. The response IS the acknowledgement.",
    steps: ["Go to PEARL. It asks PRINT LOG? Y/N.",
            "Press N - the letter should not appear, and a hint line should print instead.",
            "Press Y at the same question - the log should open."],
    ref: "spec §10.2b rule 3, the stated exception" },
  { id: "R3-7", title: "Enter at an empty prompt says something",
    expect: "Pressing Enter with nothing typed gives a response rather than silence - the prompt tells you what it wants.",
    steps: ["On SWAN at an idle prompt press Enter with an empty line.",
            "A hint should print."],
    ref: "spec §10.2b rule 3 (bare-prompt response)" } ] },

{ id: "r4", title: "RULE 4 — OUTPUT TELETYPES AT ONE MACHINE CADENCE",
  note: "The terminal prints at 45 characters a second. Input echoes instantly. Any key finishes an in-progress print at once. The Pearl printer and chat are different machines and run at their own stated rates.",
  rows: [
  { id: "R4-1", title: "Printed lines type themselves out",
    expect: "When the terminal prints a line it appears character by character at a steady, readable pace - roughly 45 characters a second, so a short hint takes well under a second.",
    steps: ["Go to PEARL and press N so it prints its hint line.",
            "Watch the line arrive - it should type out, not appear all at once."],
    ref: "spec §10.2b rule 4, TELETYPE_CPS = 45" },
  { id: "R4-2", title: "Your own typing is instant, not teletyped",
    expect: "Characters you type appear the moment you press the key. The teletype pace is for the machine's output only.",
    steps: ["Type a run of digits quickly on SWAN.", "They should keep up with you exactly."],
    ref: "spec §10.2b rule 4" },
  { id: "R4-3", title: "Any key finishes a print immediately",
    expect: "If a line is still typing out, pressing any key completes it at once. A flourish never makes you wait.",
    steps: ["Trigger a printed line (e.g. N at the Pearl prompt).",
            "While it is still typing, press a key.", "The rest of the line should appear instantly."],
    ref: "spec §10.2b rule 4" },
  { id: "R4-4", title: "The Pearl's printout runs much faster, on purpose",
    expect: "The Pearl log prints like a dot-matrix printer - noticeably faster than the terminal's own typing. That difference is intended and documented, not a glitch.",
    steps: ["Open the Pearl log with Y.", "Compare its printing speed to a terminal hint line.",
            "The log should be much faster."],
    ref: "spec §10.2b rule 4, the producers table" } ] },

{ id: "stations", title: "THE THREE STATIONS",
  note: "SWAN, PEARL and FLAME share the screen but never at each other's expense. No station's commands reach another's prompt.",
  rows: [
  { id: "S-1", title: "Each station names itself correctly",
    expect: "The header reads STATION 3 · THE SWAN, STATION 4 · THE FLAME and STATION 5 · THE PEARL.",
    steps: ["Visit each station in turn and read the header line."],
    ref: "spec §10.2b (station numbers, verified against Lostpedia's station list)" },
  { id: "S-2", title: "Each station opens with its own line",
    expect: "SWAN opens with a hint about the Numbers (or with the show's silence during a countdown). PEARL opens with PRINT LOG? Y/N. FLAME opens with CHESS? Y/N.",
    steps: ["Switch between the three stations and read the first line each prints."],
    ref: "spec §10.2b, the station table" },
  { id: "S-3", title: "The Flame's command does not work on the Swan or the Pearl",
    expect: "Typing CHESS anywhere except the Flame does nothing but echo. No chessboard appears.",
    steps: ["On SWAN type CHESS - no board.", "On PEARL type CHESS - no board.", "On FLAME type CHESS - the board appears."],
    ref: "spec §10.2b (no station's commands reach another's prompt)" },
  { id: "S-4", title: "The Pearl's command does not work on the Swan or the Flame",
    expect: "Typing LOG anywhere except the Pearl does nothing but echo. No log printout appears.",
    steps: ["On SWAN type LOG - nothing.", "On FLAME type LOG - nothing.", "On PEARL type LOG - it offers the printout."],
    ref: "spec §10.2b" },
  { id: "S-5", title: "Typing a station name switches to it",
    expect: "Typing SWAN, PEARL or FLAME at an idle prompt switches station, exactly as pressing the button does.",
    steps: ["At an idle prompt type PEARL.", "The station should change and its opening line print."],
    ref: "spec §10.2b" },
  { id: "S-6", title: "The Pearl and the Flame ignore the countdown entirely",
    expect: "Only the Swan reacts to a countdown. With a run in progress - even at zero - the Pearl and the Flame behave exactly as they do when nothing is running: no readout, no going inert, no SYSTEM FAILURE.",
    steps: ["Start a countdown.", "Switch to PEARL and to FLAME.",
            "Both should be completely normal and usable throughout."],
    ref: "spec §10.2b (Pearl and Flame are indifferent to countdown state)" },
  { id: "S-7", title: "The station is remembered per browser",
    expect: "The station you last chose is the one you get on the next load, in this browser.",
    steps: ["Select FLAME.", "Reload.", "It should still be FLAME."],
    ref: "spec §10.2b" } ] },

{ id: "boot", title: "THE BOOT ANIMATION",
  note: "It plays on every load while the station is SWAN, in both content modes, and is always skippable.",
  rows: [
  { id: "B-1", title: "It plays on an ordinary load",
    expect: "Opening or reloading /terminal.html with station SWAN plays the mark.",
    steps: ["Make sure the station is SWAN.", "Reload the page.", "The animation should play."],
    ref: "spec §10.2b (the boot animation)" },
  { id: "B-2", title: "It plays in the station screen too",
    expect: "The animation is not limited to the friendly terminal - it plays on load in the station screen as well.",
    steps: ["Turn PROTOCOL on, station SWAN.", "Reload the page.", "The animation should play."],
    ref: "spec §10.2b" },
  { id: "B-3", title: "It plays on a browser that has never seen the page",
    expect: "A fresh browser profile - or a private window - still gets the animation. It is not hidden behind a setting anyone has to find first.",
    steps: ["Open /terminal.html in a private window.", "The animation should play."],
    ref: "spec §10.2b; §17 2026-08-25 defect 1" },
  { id: "B-4", title: "It can always be skipped",
    expect: "Pressing a key or clicking during the animation ends it immediately and leaves the page usable.",
    steps: ["Reload and press a key while the mark is drawing.", "It should end at once."],
    ref: "spec §10.2b (always skippable)" },
  { id: "B-5", title: "REPLAY LOGO plays it again",
    expect: "The strip's REPLAY LOGO button replays the animation on demand.",
    steps: ["Reveal the strip and press REPLAY LOGO."],
    ref: "spec §10.2b (deliberate replays)" },
  { id: "B-6", title: "Typing LOGO plays it again",
    expect: "LOGO typed at the Swan prompt replays the animation.",
    steps: ["On SWAN at an idle prompt type LOGO."],
    ref: "spec §10.2b" },
  { id: "B-7", title: "It does NOT play on the other stations",
    expect: "Loading the page with PEARL or FLAME selected plays nothing. The Swan mark is the only artwork that exists, so the animation is Swan-only.",
    steps: ["Select PEARL, reload - no animation.", "Select FLAME, reload - no animation."],
    ref: "spec §10.2b (Pearl and Flame marks are optional future art)" },
  { id: "B-8", title: "It does NOT play on the control panel",
    expect: "The main control panel never plays the animation.",
    steps: ["Open the control panel (/ or /index.html) a few times."],
    ref: "spec §10.2b" },
  { id: "B-9", title: "It does NOT replay when the connection drops and returns",
    expect: "If the display's socket drops and reconnects, the animation must not replay - a dropped connection is not a boot.",
    steps: ["Leave /terminal.html open on SWAN.",
            "Interrupt the connection briefly (unplug the board's power or drop WiFi) and let it come back.",
            "The page should recover without replaying the mark."],
    ref: "spec §10.2b (not on socket reconnect)" } ] },

{ id: "readout", title: "THE READOUT FOLLOWS THE MODE",
  note: "A countdown number captioned as a clock is worse than either number alone.",
  rows: [
  { id: "D-1", title: "In clock mode the big readout is the real time",
    expect: "With the display in clock mode the presentation readout shows the actual current time - to the real minute, even though the flaps themselves only move every 15 minutes.",
    steps: ["Set the display to clock mode.", "Compare the big readout to a watch."],
    ref: "spec §7.4a; §17 2026-08-24 (the CRT reads the actual minute)" },
  { id: "D-2", title: "In countdown mode it is the remaining time",
    expect: "In countdown mode the readout is the countdown, captioned as such.",
    steps: ["Switch to countdown mode.", "The readout should be the remaining time."],
    ref: "spec §7.4a" },
  { id: "D-3", title: "In message mode it is dashes",
    expect: "In message mode the readout shows dashes rather than borrowing a number from somewhere else.",
    steps: ["Set a message from the control panel.", "Look at the presentation readout."],
    ref: "spec §7.4a" },
  { id: "D-4", title: "A countdown running behind the clock shows as a small chip",
    expect: "If a countdown is running while the display shows the clock, the big readout stays the clock and a separate small chip reads COUNTDOWN followed by the remaining time.",
    steps: ["Start a countdown, then switch the display to clock mode.",
            "The big readout should be the time; a COUNTDOWN chip should appear."],
    ref: "spec §7.4a" },
  { id: "D-5", title: "The same chip appears on the control panel",
    expect: "The control panel also shows the COUNTDOWN chip when a run is live behind another mode.",
    steps: ["With a countdown running behind the clock, open the control panel.",
            "The chip should be in the status line."],
    ref: "spec §7.4a" },
  { id: "D-6", title: "The readout matches the flaps",
    expect: "Whatever the readout says the countdown is, the drums should be showing the same value (allowing for a flap still in motion).",
    steps: ["During a countdown compare the readout with the mirrored columns."],
    ref: "spec §7.3 (the rendering contract, shared by all three surfaces)" } ] },

{ id: "cd", title: "THE COUNTDOWN, END TO END",
  note: "The deadline is absolute. The display never claims less time than actually remains.",
  rows: [
  { id: "C-1", title: "The Numbers start it",
    expect: "Entering 4 8 15 16 23 42 and pressing EXECUTE starts a 108-minute countdown.",
    steps: ["Type the Numbers on the terminal and press EXECUTE (or Enter)."],
    ref: "spec §10.2a countdown.execute" },
  { id: "C-2", title: "Wrong numbers are refused",
    expect: "Any other sequence is rejected and says so. Nothing starts.",
    steps: ["Type 1 2 3 4 5 6 and EXECUTE.", "It should be refused with a message."],
    ref: "spec §7.3 (wrong numbers -> rejected)" },
  { id: "C-3", title: "108:00 is held for a full minute",
    expect: "After starting, the display reads 108:00 for a whole minute before changing to 107:00. It does not roll over immediately.",
    steps: ["Start a countdown and watch the readout and the flaps for the first 70 seconds."],
    ref: "spec §7.3 (round UP, not down)" },
  { id: "C-4", title: "The seconds stay on 00 for most of the run",
    expect: "Above the four-minute mark the two seconds columns sit on 00 and only the minutes change. This saves the flaps and is deliberate.",
    steps: ["During the quiet part of a run, watch the last two columns for a minute or two."],
    ref: "spec §7.3 (the seconds freeze)" },
  { id: "C-5", title: "At four minutes the seconds come alive and the warning sounds",
    expect: "At exactly 4:00 remaining the display reads 004:00, the seconds begin ticking every second, and the four-minute warning cue plays. Nothing repeats and nothing jumps.",
    steps: ["Let a run reach 4:00 (or set a deadline about five minutes out).",
            "Watch the transition from 005:00 to 004:00 to 003:59."],
    ref: "spec §7.3 (the transition is seamless)" },
  { id: "C-6", title: "The one-minute warning sounds",
    expect: "At 1:00 remaining the one-minute cue plays.",
    steps: ["Let a run reach 1:00 with the volume up."],
    ref: "spec §7.3 cues" },
  { id: "C-7", title: "000:00 lands exactly at zero, with the alarm",
    expect: "The display reaches 000:00 at the same instant the system-failure alarm starts - not a second before it.",
    steps: ["Watch and listen at the end of a run."],
    ref: "spec §7.3 (000:00 lands exactly at remaining = 0)" },
  { id: "C-8", title: "The zero choreography: hold, spin, reveal",
    expect: "At zero the display holds 000:00 for about three seconds, then all five columns spin for about six seconds, then land on the five hieroglyphs.",
    steps: ["Watch the whole sequence after zero and time it roughly."],
    ref: "spec §7.3, countdown.zero_hold_s = 3, countdown.spin_s = 6" },
  { id: "C-9", title: "It stays on the hieroglyphs",
    expect: "After the reveal the display stays there until the mode is changed or the Numbers are entered again. It does not drift back to the clock on its own.",
    steps: ["Leave it on the reveal for a few minutes."],
    ref: "spec §7.3 (no auto-return unless configured)" },
  { id: "C-10", title: "SYSTEM FAILURE floods the station screen at zero",
    expect: "On the Swan station screen, from zero the words SYSTEM FAILURE repeat continuously and rapidly, running together with no gaps and no line breaks, filling and scrolling the screen.",
    steps: ["Have PROTOCOL on and station SWAN as a run reaches zero."],
    ref: "spec §7.3 (the SYSTEM FAILURE flood)" },
  { id: "C-11", title: "EXECUTE mid-run resets to 108:00",
    expect: "Entering the Numbers again during a run restarts it at 108:00 rather than doing nothing.",
    steps: ["Part way through a run, enter the Numbers again."],
    ref: "spec §10.2a countdown.execute (start or mid-run reset)" },
  { id: "C-12", title: "A countdown survives a power cut",
    expect: "Pulling the power mid-run and restoring it resumes the same deadline - the display comes back showing the correct remaining time, not a fresh 108:00.",
    steps: ["Start a run, note the time remaining, pull power for ten seconds, restore it.",
            "Allow a moment for the clock to sync, then compare."],
    ref: "spec §7.3 (the countdown is a deadline, not a timer)" },
  { id: "C-13", title: "A run that ended while powered off does not replay the alarm",
    expect: "If the deadline passes while the display is off, powering it back on shows the hieroglyphs quietly. No alarm, no spin - that moment has passed.",
    steps: ["Set a deadline a minute out, power off before it, wait, power on."],
    ref: "spec §17 2026-08-21 (wakes silently into the reveal)" },
  { id: "C-14", title: "A background run takes the display back at four minutes",
    expect: "If a countdown is running while the display shows the clock, then at four minutes remaining the display switches itself to countdown mode so the ending is not missed.",
    steps: ["Start a countdown, switch to clock mode, and wait for 4:00 remaining.",
            "The display should switch to countdown mode on its own."],
    ref: "spec §7.4a (the finale must never fire invisibly)" },
  { id: "C-15", title: "Cancel stops it",
    expect: "CANCEL ends the run and the display returns to normal. Nothing fires later.",
    steps: ["Start a run and cancel it.", "Wait past the point it would have ended."],
    ref: "spec §10.2a countdown.cancel" } ] },

{ id: "presets", title: "PRESETS AND THE MIRROR",
  rows: [
  { id: "P-1", title: "?????  on all five columns",
    expect: "The qmarks preset puts a question mark on every column.",
    steps: ["Control panel: choose the ????? preset.", "All five drums should show a question mark."],
    ref: "spec §7.3, §10.2a preset.set qmarks" },
  { id: "P-2", title: "Blank",
    expect: "The blank preset leaves all five columns showing the blank flap.",
    steps: ["Choose the blank preset."],
    ref: "spec §10.2a preset.set blank" },
  { id: "P-3", title: "Reveal",
    expect: "The reveal preset shows the five canon hieroglyphs - the same frame a countdown lands on at the end.",
    steps: ["Choose the reveal preset.", "Compare with what a finished countdown shows."],
    ref: "spec §11 countdown.reveal (staff, spiral, obelisk, bird, branch)" },
  { id: "P-4", title: "WiFi glyph",
    expect: "The wifi preset shows the WiFi symbol on the centre column and blanks elsewhere.",
    steps: ["Choose the wifi preset."],
    ref: "spec §7.1 (the centre column carries the wifi glyph)" },
  { id: "P-5", title: "The mirror matches the wall",
    expect: "The docked flap mirror shows what the real columns show, including the right card colours: the minutes group dark with red glyphs, the seconds group red cards with black glyphs.",
    steps: ["With MIRROR on, compare the docked mirror with the actual display through several changes."],
    ref: "spec §17 2026-08-23 (card colours; the mirror must not lie about the wall)" },
  { id: "P-6", title: "The mirror stays visible in the station screen",
    expect: "With MIRROR on, turning PROTOCOL on keeps the docked mirror on screen - it does not vanish.",
    steps: ["MIRROR on, then PROTOCOL on."],
    ref: "spec §10.2b rule 1" },
  { id: "P-7", title: "The strip never covers the mirror",
    expect: "The toggle strip and the docked mirror do not overlap, on a phone or on a large screen.",
    steps: ["Check on a wide window and on a phone-width window, in both content modes."],
    ref: "spec §17 2026-08-25 (the strip covered the mirror dock)" },
  { id: "P-8", title: "A column that does not know where it is looks different from a blank one",
    expect: "A column that is homing or whose position is unknown renders distinctly - hatched and dimmed - rather than looking like a blank flap. They are different facts.",
    steps: ["Trigger a re-home from the control panel and watch the mirror during it."],
    ref: "spec §7.4; §17 2026-08-23" } ] },

{ id: "pearl", title: "THE PEARL — THE LOG PRINTOUT",
  rows: [
  { id: "L-1", title: "Y prints the station record",
    expect: "Answering Y to PRINT LOG? prints the display's own event history - real events with times, not invented text.",
    steps: ["Go to PEARL and press Y."],
    ref: "spec §10.2b; §12 (the persistent event journal)" },
  { id: "L-2", title: "The entries are real and recognisable",
    expect: "You should be able to recognise things you actually did: countdowns executed, cancelled, reaching zero, mode changes, boots.",
    steps: ["Do a few distinct things (start and cancel a countdown, change mode), then print the log."],
    ref: "spec §12 (the kinds)" },
  { id: "L-3", title: "Newest entries are at the end",
    expect: "The printout runs oldest first, so the most recent thing you did is at the bottom.",
    steps: ["Do something memorable, then print the log and look at the last line."],
    ref: "spec §12 (NDJSON, newest LAST)" },
  { id: "L-4", title: "A key skips to the end of the printout",
    expect: "Pressing a key while the log is printing jumps to the end rather than closing it.",
    steps: ["Open the log and press a key while it prints."],
    ref: "spec §10.2b rule 4" },
  { id: "L-5", title: "It closes with the button and with ESC",
    expect: "Both the CLOSE button and the Escape key close the printout.",
    steps: ["Close it with the button. Open again and close with Escape."],
    ref: "spec §10.2b rule 2" },
  { id: "L-6", title: "N declines and leaves a hint",
    expect: "Answering N does not print the log, and a hint tells you how to ask for it later.",
    steps: ["At PRINT LOG? press N."],
    ref: "spec §10.2b (the Pearl's hint)" } ] },

{ id: "flame", title: "THE FLAME — CHESS",
  rows: [
  { id: "F-1", title: "Y opens the board",
    expect: "Answering Y to CHESS? opens a playable chessboard.",
    steps: ["Go to FLAME and press Y."],
    ref: "spec §10.2b (the Flame's station table row)" },
  { id: "F-2", title: "It is actually playable",
    expect: "You can move a piece and the machine replies with a legal move.",
    steps: ["Play several moves for both sides."],
    ref: "spec §15 phase 7 (Flame chess)" },
  { id: "F-3", title: "Winning reaches Chang's menu",
    expect: "Beating the machine brings up the numbered menu.",
    steps: ["Win a game (or use whatever the board offers to reach the menu)."],
    ref: "spec §15 phase 7 (Chang's menu)" },
  { id: "F-4", title: "Entering 77 on the menu does something",
    expect: "Typing 77 at the menu selects that entry rather than being ignored.",
    steps: ["At the menu type 77."],
    ref: "spec §15 phase 7" },
  { id: "F-5", title: "The menu echoes what you type",
    expect: "Digits typed at the menu appear, and Backspace deletes them.",
    steps: ["Type a digit, then Backspace, then a full code."],
    ref: "spec §10.2b rule 3" },
  { id: "F-6", title: "It closes with the button and with ESC",
    expect: "Both the CLOSE control and Escape leave the board.",
    steps: ["Close with the button, reopen, close with Escape."],
    ref: "spec §10.2b rule 2" },
  { id: "F-7", title: "Chess never touches the flaps",
    expect: "Nothing you do in chess changes what the display is showing. The drums carry on with whatever mode is set.",
    steps: ["Play a game and keep an eye on the display and the mirror."],
    ref: "spec §10.2b (nothing in the pack sends a dispatcher command, except Swan's EXECUTE)" } ] },

{ id: "misc", title: "AND A FEW THINGS THAT SHOULD NOT HAPPEN",
  rows: [
  { id: "X-1", title: "One press of EXECUTE starts one countdown",
    expect: "Pressing Enter or EXECUTE once starts exactly one run. The log should show one entry, not two.",
    steps: ["From the station screen, enter the Numbers and press Enter once.",
            "Print the Pearl log and check there is a single execute entry."],
    ref: "spec §17 2026-08-25 (the double execute)" },
  { id: "X-2", title: "Typing a command does not trigger something else",
    expect: "Typing a word like CHESS does not pop up an unrelated confirmation from the ordinary terminal underneath.",
    steps: ["On the station screen type CHESS, LOG and PANEL in turn and watch for stray dialogs."],
    ref: "spec §17 2026-08-25" },
  { id: "X-3", title: "The pages keep working after being left open",
    expect: "Leave the terminal open for a long stretch - it should still be live and correct, with the clock advancing.",
    steps: ["Leave /terminal.html open for an hour or more, then come back."],
    ref: "spec §10.2 (the socket reconnects)" },
  { id: "X-4", title: "The simulated columns are impossible to miss",
    expect: "While the display is running on simulated drums it says so plainly - a visible strip on the control panel and a chip on the presentation page. You should never be in doubt.",
    steps: ["Look at the control panel and the presentation header."],
    ref: "spec §5.10 (it must be impossible to mistake for real)" },
  { id: "X-5", title: "Nothing in the presentation pack moves the flaps",
    expect: "Apart from EXECUTE on the Swan, nothing in the station screen changes what the drums are doing - not changing station, not the log, not chess, not the logo.",
    steps: ["With the display in clock mode, exercise every station feature and watch the drums."],
    ref: "spec §10.2b" } ] }
];

// ---------------------------------------------------------------------------
var STORE = "swan.qa.v1";
var VERDICTS = ["PASS", "PARTIAL", "FAIL", "N/A"];
var state = {};
var ident = { fetched: false };

function load() {
  try {
    var raw = localStorage.getItem(STORE);
    if (raw) state = JSON.parse(raw) || {};
  } catch (e) { state = {}; }
  if (!state || typeof state !== "object") state = {};
}
function save() {
  try { localStorage.setItem(STORE, JSON.stringify(state)); } catch (e) { /* private mode */ }
}
function rec(id) {
  if (!state[id]) state[id] = { v: "", c: "" };
  return state[id];
}
function allRows() {
  var out = [];
  for (var i = 0; i < SECTIONS.length; i++) {
    for (var j = 0; j < SECTIONS[i].rows.length; j++) {
      out.push({ sec: SECTIONS[i], row: SECTIONS[i].rows[j] });
    }
  }
  return out;
}

function el(tag, cls, text) {
  var e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined) e.textContent = text;
  return e;
}

function build() {
  var host = document.getElementById("rows");
  host.textContent = "";
  SECTIONS.forEach(function (sec) {
    var s = el("section");
    s.appendChild(el("h2", null, sec.title));
    if (sec.note) s.appendChild(el("p", "secnote", sec.note));
    sec.rows.forEach(function (row) {
      var box = el("div", "row");
      box.id = "row-" + row.id;
      box.appendChild(el("div", "rid", row.id + (row.known ? "  ·  KNOWN" : "")));
      box.appendChild(el("div", "rtitle", row.title));

      box.appendChild(el("span", "lbl", "EXPECTED"));
      box.appendChild(el("div", "exp", row.expect));

      box.appendChild(el("span", "lbl", "STEPS"));
      var ol = el("ol", "steps");
      row.steps.forEach(function (st) { ol.appendChild(el("li", null, st)); });
      box.appendChild(ol);

      if (row.ref) box.appendChild(el("div", "ref", row.ref));

      var vs = el("div", "verdicts");
      VERDICTS.forEach(function (v) {
        var b = el("button", "v", v);
        b.setAttribute("data-v", v);
        b.onclick = function () {
          var r = rec(row.id);
          r.v = (r.v === v) ? "" : v;
          save();
          paint(row);
          tally();
        };
        vs.appendChild(b);
      });
      box.appendChild(vs);

      var ta = el("textarea");
      ta.placeholder = "notes - what you actually saw, anything odd";
      ta.value = rec(row.id).c || "";
      ta.oninput = function () { rec(row.id).c = ta.value; save(); };
      box.appendChild(ta);

      s.appendChild(box);
    });
    host.appendChild(s);
  });
  SECTIONS.forEach(function (sec) { sec.rows.forEach(paint); });
  tally();
}

function paint(row) {
  var box = document.getElementById("row-" + row.id);
  if (!box) return;
  var v = rec(row.id).v;
  box.className = "row" + (row.known && !v ? " known" : "") +
      (v === "PASS" ? " pass" : v === "FAIL" ? " fail" : v === "PARTIAL" ? " partial" : "");
  var bs = box.querySelectorAll(".v");
  for (var i = 0; i < bs.length; i++) {
    bs[i].className = "v" + (bs[i].getAttribute("data-v") === v ? " on" : "");
  }
}

function counts() {
  var c = { PASS: 0, FAIL: 0, PARTIAL: 0, "N/A": 0, untested: 0, total: 0 };
  allRows().forEach(function (x) {
    c.total++;
    var v = rec(x.row.id).v;
    if (!v) c.untested++; else c[v]++;
  });
  return c;
}

function tally() {
  var c = counts();
  document.getElementById("tally").innerHTML =
      "<b>" + c.PASS + "</b> pass &middot; <b>" + c.FAIL + "</b> fail &middot; <b>" +
      c.PARTIAL + "</b> partial &middot; <b>" + c["N/A"] + "</b> n/a &middot; <b>" +
      c.untested + "</b> left of " + c.total;
}

// --- the device's identity -------------------------------------------------
function readIdent() {
  fetch("/api/state", { cache: "no-store" }).then(function (r) { return r.json(); }).then(function (s) {
    var sys = s.sys || {};
    ident = {
      fetched: true,
      version: sys.version || "(none reported)",
      partition: sys.ota_partition || "?",
      reset: sys.reset || "?",
      uptime_s: sys.uptime_s,
      heap: sys.heap,
      simulated: s.motion ? s.motion.simulated : undefined,
      mode: s.mode,
      time_valid: s.time_valid,
      host: location.host
    };
    document.getElementById("ident").textContent =
        "firmware " + ident.version + " on " + ident.partition +
        (ident.simulated ? "  ·  SIMULATED DRUMS" : "") +
        "  ·  " + ident.host;
  }).catch(function () {
    ident = { fetched: false, host: location.host };
    document.getElementById("ident").innerHTML =
        "<span class='warn'>/api/state unreachable - open this page from the display " +
        "(http://lost.local/qa.html) so the report can record which firmware you tested.</span>";
  });
  document.getElementById("fwnote").textContent =
      "Note: the firmware reports a version string, not a build hash - /api/ exposes no hash today. " +
      "The report records the version, the running OTA slot and the host; pair it with the release " +
      "checksums if you need to be certain which image is on the board.";
}

// --- export ----------------------------------------------------------------
function pad(n) { return (n < 10 ? "0" : "") + n; }
function stamp(d) {
  return d.getFullYear() + "-" + pad(d.getMonth() + 1) + "-" + pad(d.getDate()) +
         " " + pad(d.getHours()) + ":" + pad(d.getMinutes());
}

function markdown() {
  var c = counts();
  var L = [];
  L.push("# LOST Swan - acceptance pass");
  L.push("");
  L.push("- **When:** " + stamp(new Date()));
  L.push("- **Firmware:** " + (ident.fetched
      ? ident.version + " (slot " + ident.partition + ", reset " + ident.reset +
        (ident.simulated ? ", SIMULATED DRUMS" : "") + ")"
      : "not read - page was not served from the display"));
  if (ident.fetched) {
    L.push("- **Device:** " + ident.host + ", mode " + ident.mode +
           ", clock " + (ident.time_valid ? "synced" : "NOT synced") +
           ", heap " + ident.heap + ", up " + ident.uptime_s + " s");
  }
  L.push("- **Browser:** " + navigator.userAgent);
  L.push("- **Viewport:** " + window.innerWidth + "x" + window.innerHeight +
         " @ dpr " + (window.devicePixelRatio || 1));
  L.push("- **Result:** " + c.PASS + " pass, " + c.FAIL + " fail, " + c.PARTIAL +
         " partial, " + c["N/A"] + " n/a, " + c.untested + " not tested (of " + c.total + ")");
  L.push("");

  var fails = [];
  allRows().forEach(function (x) {
    var r = rec(x.row.id);
    if (r.v === "FAIL" || r.v === "PARTIAL") fails.push({ x: x, r: r });
  });
  if (fails.length) {
    L.push("## What failed");
    L.push("");
    fails.forEach(function (f) {
      L.push("- **" + f.r.v + " " + f.x.row.id + "** - " + f.x.row.title +
             (f.r.c ? "  — " + f.r.c.replace(/\n+/g, " ") : ""));
    });
    L.push("");
  }

  SECTIONS.forEach(function (sec) {
    L.push("## " + sec.title);
    L.push("");
    sec.rows.forEach(function (row) {
      var r = rec(row.id);
      L.push("### " + (r.v || "not tested") + " - " + row.id + " " + row.title);
      L.push("");
      L.push("*Expected:* " + row.expect);
      if (row.ref) L.push("");
      if (row.ref) L.push("*Spec:* " + row.ref);
      if (r.c) {
        L.push("");
        L.push("*Comment:* " + r.c);
      }
      L.push("");
    });
  });
  return L.join("\n");
}

// --- wiring ----------------------------------------------------------------
load();
build();
readIdent();

document.getElementById("export").onclick = function () {
  var w = document.getElementById("exportwrap");
  document.getElementById("exportmd").value = markdown();
  w.classList.add("on");
  w.scrollIntoView({ behavior: "smooth" });
};
document.getElementById("hide").onclick = function () {
  document.getElementById("exportwrap").classList.remove("on");
};
document.getElementById("copy").onclick = function () {
  var ta = document.getElementById("exportmd");
  ta.removeAttribute("readonly");
  ta.select();
  var ok = false;
  try { ok = document.execCommand("copy"); } catch (e) { ok = false; }
  ta.setAttribute("readonly", "readonly");
  if (!ok && navigator.clipboard) navigator.clipboard.writeText(ta.value);
  this.textContent = "COPIED";
  var b = this;
  setTimeout(function () { b.textContent = "COPY TO CLIPBOARD"; }, 1400);
};
document.getElementById("download").onclick = function () {
  var blob = new Blob([document.getElementById("exportmd").value], { type: "text/markdown" });
  var a = document.createElement("a");
  a.href = URL.createObjectURL(blob);
  a.download = "swan-acceptance.md";
  document.body.appendChild(a);
  a.click();
  setTimeout(function () { URL.revokeObjectURL(a.href); a.remove(); }, 500);
};
document.getElementById("jump").onclick = function () {
  var next = null;
  allRows().forEach(function (x) {
    if (!next && !rec(x.row.id).v) next = x.row.id;
  });
  if (next) {
    var box = document.getElementById("row-" + next);
    box.scrollIntoView({ behavior: "smooth", block: "center" });
  } else {
    this.textContent = "ALL DONE";
    var b = this;
    setTimeout(function () { b.textContent = "NEXT UNTESTED"; }, 1600);
  }
};
document.getElementById("reset").onclick = function () {
  if (!window.confirm("Clear every verdict and comment on this browser?")) return;
  state = {};
  save();
  build();
};
