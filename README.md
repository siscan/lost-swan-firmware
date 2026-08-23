# lost-swan-firmware

Firmware for the LOST Swan-station split-flap display.
Target: ESP32-C5-DevKitC-1-N8R8 (XIAO ESP32-C5 map behind a board define).

**Start here:** `CLAUDE.md` (working agreement) → `docs/FIRMWARE_SPEC.md` (the spec).

## Status

- **Spec v1.0** — all questions answered (spec §16); resolutions in the §17 decision log
- Hardware: DevKitC-1 on order; **chip revision unverified** (checked on first flash)
- Code: **Phase 3 complete** (web UI, WiFi STA, mDNS, `/ws` + `/api`, ring
  upload, gzipped assets in LittleFS) on top of Phase 2 (fluid ring, frame
  scheduler, modes with the deadline countdown, time service, browser
  simulator).  Nothing has been flashed or measured on hardware —
  `docs/BRINGUP.md` tracks that separately.

| gate | status |
|---|---|
| `set-target esp32c5` + `build` clean | passes — zero warnings, both board maps |
| host tests green | 9/9 suites (rings, motion math, simulated axis, ring.json, TZ/DST, frame, modes, wear, web API) |
| `git diff` empty after `tools/ringgen.py` | clean — header and ring.json both regenerate byte-identically |
| motion cross-task handoff explicit | done — see `docs/MOTION_SYNC.md`, incl. the seqlock for multi-field reads |
| CI | GitHub Actions on ubuntu — see below |
| flashed to hardware | **not done — board not arrived** |

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
| `app.js` | 16,978 | 5,488 |
| `index.html` | 12,070 | 3,909 |
| `terminal.js` | 10,418 | 3,865 |
| `flap.js` | 9,882 | 3,657 |
| `terminal.css` | 8,122 | 2,800 |
| `style.css` | 6,199 | 2,185 |
| `ring.json` | 9,361 | 1,441 |
| `bus.js` | 2,946 | 1,184 |
| `terminal.html` | 2,126 | 919 |
| **total** | **135,704** | **46,195** |

Against a **2048 KB** partition, so the room is for audio (spec §9). The
packer fails the build above a 256 KB budget rather than letting the UI
quietly eat it. The glyph sheet is 45% of the payload and worth it; digits,
AM/PM and blank are still text placeholders, and exporting those too would add
roughly 6 KB gzipped.

### Ring upload

The HTTP task validates an uploaded `ring.json` entirely into a *staging*
table — parse, exactly 50 slots, and every role its column will be asked for.
The running table is swapped in by the **modes task** at a tick boundary
(`ring_store.h`'s threading contract), and only then is the file written, via
a temp file and a rename. A malformed, truncated, oversized or
role-incomplete upload leaves the display exactly as it was and never reaches
the filesystem.

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
