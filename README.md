# lost-swan-firmware

Firmware for the LOST Swan-station split-flap display.
Target: ESP32-C5-DevKitC-1-N8R8 (XIAO ESP32-C5 map behind a board define).

**Start here:** `CLAUDE.md` (working agreement) → `docs/FIRMWARE_SPEC.md` (the spec).

## Status

- **Spec v1.0** — all questions answered (spec §16); resolutions in the §17 decision log
- Hardware: **DevKitC-1 V1.2 on the bench, chip revision v1.2** (production
  silicon; the §2.0 risk is closed). Console on **COM3**, USB-Serial/JTAG.
  Flashed and booting; nothing wired yet, so all five columns hunt for ~30 s
  and then latch FAULT with cause `no_hall`, by design.  `sim all` runs the
  whole stack against modelled drums instead, which is how Phases 4 and 5 get
  exercised on real silicon while the mechanics are weeks out
- Code: **Phase 3 complete** (web UI, WiFi STA, mDNS, `/ws` + `/api`, ring
  upload, gzipped assets in LittleFS) on top of Phase 2 (fluid ring, frame
  scheduler, modes with the deadline countdown, time service, browser
  simulator).  Nothing has been flashed or measured on hardware —
  `docs/BRINGUP.md` tracks that separately.

| gate | status |
|---|---|
| `set-target esp32c5` + `build` clean | passes — zero warnings, both board maps |
| host tests green | 10/10 C++ suites (rings, motion math, simulated axis, ring.json, TZ/DST, frame, modes, wear, fault policy, web API) plus the mirror widget's JS suite (CI; SKIPPED locally, no node) |
| release image cannot carry the simulator | `-DSWAN_RELEASE=1` with `SWAN_SIM_AXES=ON` is a configure-time `FATAL_ERROR`; CI builds both halves |
| Phase 3 adversarial review | 22 findings confirmed, all fixed — see spec §17 |
| `git diff` empty after `tools/ringgen.py` | clean — header and ring.json both regenerate byte-identically |
| motion cross-task handoff explicit | done — see `docs/MOTION_SYNC.md`, incl. the seqlock for multi-field reads |
| CI | GitHub Actions on ubuntu — see below |
| flashed to hardware | **done 2026-08-23** — boots, CLI up, ring.json loads from LittleFS, unwired homing faults cleanly |
| `/ws` verified on silicon | **done** — client registers, 1 Hz heartbeat, live state (the dev server cannot test this) |
| ring upload vs real LittleFS | **done** — broken files rejected in 0.00 s, a valid one persists and drives resolution |
| chip revision | **v1.2** — production silicon, inside the image's v1.0–v1.99 window |
| sim / disabled / maintenance on hardware | **done 2026-08-23** — five simulated drums run the real clock; slip recovers, a jam latches without retrying, a disabled column leaves a hole, maintenance survives a reboot. `docs/BRINGUP.md` step 17 |

## CI — the reliability source of truth

`.github/workflows/ci.yml` runs on every push to `master` and every pull
request, on **ubuntu**, where
none of the Windows dev machine's constraints exist:

| job | what |
|---|---|
| `ring-table` | `python3 tools/ringgen.py --check` — the committed header AND `data/ring.json` must match the two manifests |
| `host-tests` | native CMake build + ctest of all seven pure-logic suites, then a freshness diff of the committed simulator traces against a live `gen_traces` run |
| `firmware` | both board maps (`devkitc1`, `xiao`) built inside Espressif's official `espressif/idf:v5.5.5` Docker image |

**Linux CI is the source of truth for reliability.** The Smart-App-Control
retry in `test-host.ps1` is local convenience only — a suite that needs the
retry locally is still a clean pass, but a suite that fails on CI is a real
failure regardless of what the Windows machine says. While `ar.exe` is blocked
here, host test builds stay free of static libraries (`test/host/CMakeLists.txt`
compiles the pure sources straight into each test executable), which costs
nothing on any platform.

## Phase 1.5 — the simulated-axis suite

`test/host/test_axis_sim.cpp` drives the **real** control tick
(`components/motion/axis_control.cpp` — the same code the firmware links) and
the **real** step-ISR helpers against a modeled drum (272000/33 µsteps per
revolution, configurable Hall window width and per-edge jitter). It asserts:
homing from any start angle including inside the magnet window; the full 50×50
`go` matrix with mid-move re-basing across the edge, judged by the *drum's*
mechanical angle rather than the firmware's own bookkeeping; slip of 30 µsteps
→ minor resync, 100 → major, 200 → fault followed by a successful automatic
re-home; negative and >1-revolution calibration offsets normalising; an
8369-µstep drum (the stale 68/26 gearing) logging a major resync every
revolution and never faulting; and mailbox ordering — commands in separate
control periods all apply in order, two commands in one period apply exactly
the newer one whole.

## The board

| | |
|---|---|
| board | ESP32-C5-DevKitC-1 **V1.2** (PCB silkscreen), N8R8 |
| chip | **ESP32-C5 revision v1.2** — production silicon |
| efuse block rev | v0.3 |
| ROM | `esp32c5-eco3-20250704` |
| features | Wi-Fi 6 dual-band, BT 5 LE, 802.15.4, single core + LP core, 240 MHz |
| crystal | 48 MHz |
| base MAC | `10:bd:a3:dd:a8:e8` |
| port | **COM3**, USB-Serial/JTAG |

```powershell
.\build.ps1 -p COM3 flash
.\build.ps1 -p COM3 monitor      # Ctrl+] to exit
```

**If a future IDF bump ever prints `Image requires chip rev <= …`, that is an
IDF-version problem, not a board fault.** An image carries a supported chip
revision range and the bootloader *aborts* rather than warns outside it. This
build reports `Min chip rev: v1.0  Max chip rev: v1.99  Chip rev: v1.2`, so
v1.2 sits inside the window. ESP-IDF v5.5.5 also offers
`CONFIG_ESP32C5_REV_MIN_102`, i.e. it knows v1.2 as a shipping revision.

Bench note for scripted console work: **pyserial asserts DTR and RTS on
open**, and on USB-Serial-JTAG those drive EN and the boot pin — so opening the
port per command silently resets the board between commands. Clear both before
`open()`.

## Pinned versions

| what | version | notes |
|---|---|---|
| ESP-IDF | **v5.5.5** | `git clone -b v5.5.5 --depth 1 --recursive --shallow-submodules` into `~/esp/esp-idf` |
| Toolchain | riscv32-esp-elf **14.2.0_20260121** | installed by `install.ps1 -Targets esp32c5` |
| IDF CMake / Ninja | 3.30.2 / 1.12.1 | from `~/.espressif/tools` |
| Python | 3.13.15 | `winget install Python.Python.3.13 --scope user` |
| Host compiler | MinGW-w64 GCC **16.1.0** (WinLibs POSIX UCRT) | `winget install BrechtSanders.WinLibs.POSIX.UCRT --scope user`; host tests only |
| Board | ESP32-C5-DevKitC-1-N8R8 | ordered 2026-08-21 |
| Chip revision | **unknown** — first `idf.py monitor` prints it; below v1.0 goes back | |

## Activating the toolchain

This is a **Windows** machine. `install.sh` / `export.sh` do not work here —
ESP-IDF dropped MSys/Mingw support at v4.0 and `install.sh` refuses outright
with `ERROR: MSys/Mingw is not supported`. The activation command is:

```powershell
. $HOME\esp\esp-idf\export.ps1
```

`build.ps1` does this for you, so you rarely need it by hand.

## Build — use `build.ps1`

`build.ps1` sources ESP-IDF and forwards everything to `idf.py`, so activation
can never be forgotten or half-applied. **This is the documented build command.**

```powershell
.\build.ps1 set-target esp32c5
.\build.ps1                          # same as: idf.py build
.\build.ps1 -p COM5 flash monitor
.\build.ps1 -B build-xiao -DSWAN_BOARD=xiao build
```

Run it from PowerShell. A Git Bash shell cannot host ESP-IDF at all, so call it
across from Bash if you need to:

```bash
powershell -ExecutionPolicy Bypass -File ./build.ps1 build
```

## Host unit tests — use `test-host.ps1`

```powershell
.\test-host.ps1                                   # builds it
.\test-host.ps1                                   # builds it
```

It uses the CMake and Ninja that `install.ps1` already put under
`~/.espressif/tools` — no separate CMake install — plus the user-scope MinGW-w64
GCC from winget. No Visual Studio, nothing needing admin.

Three machine constraints are baked into that script and into
`test/host/CMakeLists.txt` — all one root cause: **Smart App Control is
enforced on this machine** (`HKLM:...\CI\Policy\VerifiedAndReputablePolicyState
= 1`) and blocks unsigned binaries it has no reputation for:

- **`ar.exe` is permanently blocked** (WinLibs' bundled `cmake.exe` too). So the
  suite compiles the pure sources straight into each test executable rather
  than building a static library, and uses the **Ninja** generator — MinGW
  Makefiles archives objects with `ar` before linking and can never work here.
- **Freshly linked test binaries are blocked at random, by hash.** The verdict
  is cached per hash (moving the file changes nothing), but a relink embeds a
  new timestamp, gets a new hash, and usually passes. ctest reports a blocked
  binary as `BAD_COMMAND` / `Not Run` — not as a test failure — and
  `test-host.ps1` detects exactly that and relinks — **only the binaries that
  were actually blocked**, keeping the ones that ran. Relinking the whole set
  each round needs every binary to clear the coin-flip at once, which stopped
  converging past a handful of tests. A real test failure is never retried.
- The winget tools are not on the inherited PATH, so the script locates them.

The clean fix would be turning Smart App Control off (Settings → Privacy &
security → Windows Security → App & browser control) — a decision for Nico, not
the firmware: it needs admin, and **once off it cannot be re-enabled without
reinstalling Windows**. The retry loop makes the tests reliable without it.

On a machine without those constraints the plain form in CLAUDE.md works as-is:

```bash
cmake -S test/host -B build_host && cmake --build build_host && ctest --test-dir build_host --output-on-failure
```

## Ring table

**Ring v3 is descending, and there are two rings.**  Ring A drives columns 1–4;
ring B drives column 5 and carries each digit **twice** (slots 15–24 and 40–49)
so its 0→9 wrap costs 16 flips instead of 41.  One forward flip *decrements* the
digit, which makes a countdown tick a single flip on every column — and makes a
clock tick the expensive direction, hence `clock.granularity_min` (default 15).
Because a digit has no single slot on column 5, every lookup resolves to the
nearest match **going forward**; column 5's physical position is therefore not
predictable from its displayed digit (see `docs/BRINGUP.md`).

The generator is `tools/ringgen.py`; it emits BOTH the compiled fallback header
(two tables plus the per-column assignment) and `data/ring.json` (the runtime
table for LittleFS), and validates descending order and per-column role coverage
as it goes.  Regenerate after any change to either manifest in `docs/ref/`:

```powershell
python tools/ringgen.py
```

`--check` fails if either committed artifact is stale; CI runs it.
`gen_ring_table.py` remains as a forwarding shim.

A ring table that cannot render a role its column will be asked for — column 1
without AM/PM, the centre column without the wifi glyph, any column missing a
digit or `?` — is **rejected at load**, leaving the compiled fallback active, so
a bad upload fails at boot rather than as a blank column mid-show.

## Web UI and the host dev server

The UI is vanilla HTML/CSS/JS in `web/` — no framework, no web fonts, every
byte ships in LittleFS. Pages: **Terminal** (the Numbers + EXECUTE, live
mirror, remaining), **Modes**, **Calibrate**, **Diagnostics**, **Settings**
(TZ, granularity, seconds mode, the reveal picker, ring upload) and
**Update** (a stub until OTA lands in Phase 4).

Every control sends a §10.2a command; there is no second control path. What
the page can do, an MQTT publish will be able to do, and the firmware
validates both identically.

**Click through it with no hardware.** `tools/devserver/` is a host binary
that serves `web/` and speaks the *real* `/ws` against the *real* ModeManager,
FrameScheduler and dispatcher, over five simulated axes running the *real*
control core (the same `sim::SimAxis` the Phase 1.5 suite drives — real
homing, real edge verification, real forward-only motion):

```powershell
.\test-host.ps1                                   # builds it
build_host\devserver.exe --port 8080 --root web --ring data/ring.json
```

then open **http://localhost:8080/**. It homes all five columns first (about
six seconds, visible), then the clock runs. `--tz` overrides the timezone.

Routes, identical on the host and on the device:

| route | what |
|---|---|
| `GET /` | the UI (gzipped assets preferred) |
| `GET /api/state` | the full state document |
| `GET /api/ring` | per-column ring tables, glyph lists and colour schemes |
| `GET /api/wear` | measured flips/day per granularity and flips/run per seconds mode |
| `POST /api/cmd` | one §10.2a command, JSON body |
| `POST /api/ring/upload` | a candidate `ring.json`, raw body |
| `/ws` | state on change + 1 Hz heartbeat, plus `go`/`spin`/`mode`/`cue` events |

Every `/ws` message carries an `"e"` discriminator, and `web/flap.js` renders
both the live stream and a replayed simulator trace — one renderer, so the two
cannot drift apart. `web/bus.js` is the one transport, shared by the control
panel and the presentation terminal.

### Presentation terminal — `/terminal.html`

A separate fullscreen page (spec §15 phase 3.5), entirely browser-side: the
Swan terminal face with a large countdown readout, an on-screen keypad for the
Numbers, and the flap display docked small in a corner. The CRT effect
(scanlines, phosphor glow, slight barrel, subtle flicker) defaults **off** —
it costs readability and the compositing is real work on a phone GPU — and key
clicks are synthesized in WebAudio rather than shipped as samples. Preferences
persist in `localStorage`, per browser, never in NVS.

It is laid out for a **kiosk screen** and scales down, because it is a
candidate implementation of the terminal prop: a Pi in kiosk mode pointed at
`lost.local`. Built that way the prop needs no MQTT and no second countdown
implementation — it is the display's own page, so the deadline it renders is
the display's by construction.

### Glyphs

`web/glyphs.svg` carries 37 `<symbol>`s (36 glyphs plus wifi) on a shared
`0 0 100 127` viewBox, ids matching the manifest names. It is fetched once and
**injected into the document**, so every `<use>` is a same-document reference:
external references (`<use href="sheet.svg#id">`) are unsupported in WebKit
and fail over `file://`, which would mean a display that works on desktop
Chrome and is blank on an iPhone. The ids are namespaced on injection
(`swan-glyph-…`) because names like `sun`, `hand` and `wave` would otherwise
join the page's single id space.

Glyphs are never stretched — the 100:127 box is the flap card's own ratio and
each glyph is pre-scaled inside it — and take their colour from
`fill: currentColor` under the per-column scheme.

### Asset budget

`tools/webpack.py` gzips the assets and stages `ring.json` beside them; the
build turns that into `storage.bin`. Only the `.gz` copies ship, and
`components/net/httpd.cpp` serves them with `Content-Encoding: gzip`.

| file | raw | gzipped |
|---|---:|---:|
| `glyphs.svg` | 57,602 | 20,747 |
| `ring.json` | 9,361 | 9,361 |
| `app.js` | 23,650 | 7,816 |
| `index.html` | 13,852 | 4,590 |
| `terminal.js` | 12,024 | 4,481 |
| `flap.js` | 11,572 | 4,151 |
| `terminal.css` | 9,044 | 3,085 |
| `style.css` | 8,347 | 2,916 |
| `bus.js` | 2,946 | 1,184 |
| `terminal.html` | 2,274 | 953 |
| **total** | **150,672** | **59,284** |

Against a **2048 KB** partition, so the room is for audio (spec §9). The
packer fails the build above a 256 KB budget rather than letting the UI
quietly eat it. The glyph sheet is 37% of the payload and worth it; digits,
AM/PM and blank are still text placeholders, and exporting those too would add
roughly 6 KB gzipped. `ring.json` is the one file that ships **uncompressed** —
the firmware opens it directly and knows nothing about gzip.

### Per-column mode: real, sim, disabled

Each column is independently `real`, `sim` or `disabled`, persisted in NVS and
set from the console or Settings → Columns.  The two configurations this exists
for: **one real column and four simulated** during build-out, and **one
disabled and four real** during a repair.

```
col                      # list all five
col 0 real               # this column drives hardware
sim all                  # every column simulated
sim 2 off                # column 2 back to real
col 3 disabled           # parked on blank, left out of every frame
sim fault 0 slip 200     # inject a slip on a simulated column
sim fault 0 miss 2       # suppress two hall edges -> classified as a jam
sim fault 0 clear        # drop injected faults
maint on                 # suspend everything and release EN

```

A **simulated** column runs the *real* control core, the real 1 kHz tick and
the real 50 kHz step ISR — only the Hall input comes from a modelled drum
(`motion/sim_drum.h`, division-free so it is safe in the IRAM ISR, using the
real 272000/33 µsteps/rev so a homing pass takes the real ~7.5 s).  Modes,
frames, ring, countdown, scheduler and the whole web UI are the same code on
the same path.

It is **impossible to mistake for real**: an `ESP_LOGW` line at boot, a
`motion.simulated` field in the state payload, a permanent amber strip in the
web UI, a chip on the presentation terminal, and the mode per column in
`stats`.  It is **never the default** — `ColumnConfig`'s defaults are all-real
(`static_assert`ed), and `-DSWAN_RELEASE=1` with `SWAN_SIM_AXES` still ON is a
configure-time `FATAL_ERROR`, so a release image cannot ship able to simulate.

A **disabled** column is parked on blank, excluded from every frame, never
homed, never retried, and reported as *configuration* rather than as a fault.
It is set by you and never inferred: no fault ever disables a column.  The mode
keeps running and the display carries a hole — a clock missing one digit tells
you more than a dark display.

### Fault causes, and why a jam is not retried

| cause | signature | response |
|---|---|---|
| `no_hall` | a homing pass saw **no edge at all** | retry (the drum is turning freely; a pass costs only time) |
| `slip` | the edge **arrived**, more than a flap out | retry (a re-home is the recovery) |
| `jam` | an expected edge **never came** while the motor kept stepping | **stop at once, no retry** |

Retrying a jam drives a stepper into an obstruction for 7.5 s at a time against
printed gear teeth.  Escalation: one column on a sensor signature parks and the
rest keep running; a jam stops that column immediately; **two or more columns
faulted, or any fault during the alarm spin, drops EN for all five**.

**EN is ganged.**  One GPIO drives all five drivers and the pin map has exactly
one spare non-strapping GPIO, so per-column de-energize does not exist.
Parking or stopping a column stops it *stepping* — its coils still hold
standstill current.  `en 0`, or maintenance mode, is the only true de-energize
and it takes the whole display.

### Maintenance mode

`maint on` (or Settings → Maintenance) stops frame scheduling, suspends the
modes, holds cues, turns off automatic re-homing and releases EN.  Manual
commands still work regardless of fault state, so a suspect column is driven by
hand from the Calibrate page.  It **survives a reboot** deliberately: pulling
power mid-repair must not restart a countdown on top of your hands.  `maint
off` re-arms everything and re-homes all five.

### Card colours

From the manifests' `part_note`, which is the authority: cols 1-3 (minutes) are
black cards with white inverted digits and red glyphs; cols 4-5 (seconds) are
white clock cards with a **red glyph block printed in black ink**.  Settled
2026-08-23 after being changed and changed back — see the spec §17 log rather
than re-deriving it.

The straddle flaps the manifests list per slot are not rendered: they stop
colour leak between a dark face and a light one, which is a print-side fix, and
a straddle flap is half of two adjacent cards so it has no single card colour.

### The mirror tracks the state document, not the event stream

`go`/`spin` events drive the flip *animation*.  They are not the source of
truth and must never be treated as one: they travel `/ws`, which is
best-effort, and until 2026-08-23 the board delivered **two of every five** —
`httpd_queue_work` posts over a control socket whose mbox is six deep, and
queuing one job per client per message overran it on every frame.  Nothing
corrected the drift, because the frame scheduler does not re-command a column
already at its target, so one lost event left that card wrong for ever.

Two fixes, and both matter:

- `components/net/httpd.cpp` keeps **one** outbound queue and **one** drain job
  in flight, whatever the message rate and however many clients.  Drops are
  counted and published as `sys.ws_dropped` (Diagnostics) — the silence is what
  made the bug invisible.
- `web/flap.js` `reconcile(cols, flaps)` runs on every state document and
  brings each card to the axis's authoritative face, animating forward rather
  than snapping.  `index < 0` beats `dest`: a column hunting for its hall edge
  must render as unknown, not as a confident blank.

So a dropped event now costs an animation, never correctness.
`test/host/test_flap.js` pins it by driving frames with events deliberately
missing.

One trap worth knowing if you touch this path: `send_wait_timeout` is 1 s, and
lwIP treats **any** non-zero `SO_SNDTIMEO` as *never block*, so a short TCP send
buffer fails immediately with `EWOULDBLOCK` on a perfectly healthy client — a
phone whose radio naps is enough.  A failed send therefore **closes** the socket
rather than merely unregistering it: leaving it open would give the browser a
connection it believes in and no state documents, which silently switches the
reconcile back off.  Raising the timeout does not help; with `dontblock` set it
changes nothing except how long a slow client can stall the single httpd task.

### Motion state is always visible

A column whose index is `-1` is **not** showing the blank flap — it is
unhomed, hunting for its hall edge, or freshly spun open-loop. Those used to
render identically, so a display that was actively searching looked like one
sitting idle. Unknown now renders hatched and dimmed, pulses while hunting,
and a persistent banner (outside `<main>`, and a header chip on the
presentation terminal) names the columns and the re-home attempt.

Worth knowing at the bench: a homing pass is **~7.5 s** and a column tries
**three** times, so there is a **~30 s window from boot** where the display is
searching. The banner is what makes that legible.

### Ring upload

The HTTP task validates an uploaded `ring.json` entirely into a *staging*
table — parse, node budget, exactly 50 slots, and every role its column will
be asked for. The running table is swapped in by the **modes task**, through
`ModeManager::cmd_ring_swap` so it holds the same lock every command takes,
and only then is the file written by renaming a temp file over the old one. A
malformed, truncated, oversized or role-incomplete upload leaves the display
exactly as it was and never reaches the filesystem.

Readers outside the modes task — four HTTP routes, three CLI commands — take a
**pinned snapshot** rather than a reference: the swap frees the outgoing
tables, and a `const RingTable&` held across a response would be reading freed
heap. `ring_store.h` carries the full contract.

### WiFi

Credentials come from the console this phase; captive-portal provisioning is
Phase 4.

```
wifi <ssid> <password>     # saves to NVS and connects
wifi status
wifi clear
```

With no credentials the display is a standalone clock (spec §10.0): SNTP never
syncs, and the centre column shows the WiFi glyph after the 15 s grace period.
That is specified behaviour, not a fault.

## Browser simulator

Open `web/sim/index.html` (any static server, or file:// in most browsers).
It renders the five columns from `ring.json` in their per-column colour
schemes and replays traces recorded from the REAL ModeManager/FrameScheduler
by `test/host/gen_traces.cpp` — the page animates what the firmware logic
actually decided, through a MockSocket speaking the intended Phase 3 `/ws`
shape.  Refresh the traces after mode/frame changes:

```powershell
build_host\gen_traces.exe data\ring.json web\sim\traces.js
```

## Pin map (DevKitC-1, spec §2.2)

| signal | GPIO |
|---|---|
| STEP 1–5 | 8, 9, 10, 11, 12 |
| EN (ganged, active low) | 6 |
| HALL 1–5 | 0, 1, 4, 5, 23 |
| I2S BCLK / LRCLK / DIN | 7, 25, 26 |
| BUTTON | 28 (onboard BOOT, external in parallel) |
| LED | 27 (WS2812) |
| DIR | **tied at the drivers — no GPIO** |

Only I2S sits on strapping pins (2, 3, 7, 25, 26, 27, 28), and only because the
amp inputs are high-Z. `components/swan_hal/include/hal/pins.h` `static_assert`s
that no STEP, EN or HALL pin lands on one, and that no GPIO is used twice — for
either board. `pins` on the console prints the resolved map.

## Dependencies

| managed component | reason |
|---|---|
| `espressif/led_strip` | WS2812 status LED on the DevKitC-1. The XIAO path is plain GPIO and pulls nothing in. |
| `joltwallet/littlefs` | the `storage` filesystem for `ring.json`, the web assets and audio later (spec §4/§9/§10.2). Its image builder is pure Python (`littlefs-python` in a venv), so it works on this machine. |
| `espressif/mdns` | `http://lost.local/` without anyone reading an IP off a router page (spec §10.1). First-party; there is no in-tree mDNS. |

Nothing else. The host tests deliberately have no test framework — three macros
in `test/host/check.h` cover what they need.

## How motion synchronises across tasks

The full contract - ownership table, atomics and memory orders, critical
sections, IRAM/DRAM placement - is one page: **`docs/MOTION_SYNC.md`**.  The
control logic itself is the pure core `components/motion/axis_control.cpp`
(host-tested by the simulated-axis suite); `motion.cpp` is the IDF shell that
owns every lock, the ISR, and the task.

## Notes

- **`components/hal/` is `components/swan_hal/`** — ESP-IDF ships a first-party
  component named `hal` and a same-named project component shadows it. Headers
  keep the `hal/` prefix, so includes still read `#include "hal/pins.h"`.
  (CLAUDE.md's layout and its "never name a project component after an IDF one"
  rule now say the same.)
- **NVS keys are not the spec §11 names.** NVS caps a key at 15 characters and
  `motion.flaps_s_normal` is 21. The spec names stay the user-visible API; the
  short storage keys exist only in `components/config/config.cpp`, which carries
  the mapping table.

## Layout

```
components/swan_hal/   pin map, GPIO bank writes, status LED
components/ring/       geometry constants, T(i), ring table (generated), index math
components/motion/     step ISR, axis FSM, homing, edge verification, calibration
components/frame/      frame scheduler (land-on-tick, convergence after re-home)
components/modes/      clock, message, deadline countdown, the §10.2a dispatcher
components/timesvc/    SNTP + an owned POSIX-TZ engine
components/webapi/     pure state payload, command dispatch, ring upload staging
components/net/        WiFi STA, mDNS, esp_http_server (/ws + /api)
components/config/     NVS schema and defaults
components/cli/        bring-up console (spec §13)
main/                  boot sequence
web/                   the UI (index.html, app.js, flap.js, style.css)
web/sim/               browser simulator, replays recorded real-logic traces
tools/                 ringgen.py, webpack.py, the host dev server
test/host/             host unit tests
```

`components/ring/` and `components/motion/include/motion/motion_math.h` are pure
— no IDF includes — so the host tests compile them directly.
