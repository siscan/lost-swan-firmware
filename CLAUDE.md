# LOST Swan split-flap — firmware

Firmware for a wall-mounted five-column split-flap display replicating the
LOST Swan-station countdown timer. ESP32-C5-DevKitC-1-N8R8 (ordered; chip revision
unverified until arrival — spec §2.0–2.4; XIAO ESP32-C5 is the fallback map), five NEMA 17 + TMC2209 columns, Hall homing, I2S
audio, web UI + MQTT/Home Assistant.

**Read the spec first:** @docs/FIRMWARE_SPEC.md — every constant, mode, API
and open question is there. This file is the working agreement.

## How to work with Nico

- Blunt, direct technical feedback. No hedging, no cheerleading, no restating
  what he said back to him.
- He checks claims and has caught errors before. Flag uncertainty explicitly
  ("I believe X, not verified") rather than stating guesses as facts.
- **Never hard-code a value tagged `VERIFY` in the spec without asking.** Never
  resolve a `[Qn]` silently — use its default and say so, or ask if the phase
  needs the answer.
- Measurement beats assumption. If a bench result contradicts the spec
  (e.g. hall_to_hall ≠ 8242), the spec is wrong; say so and fix the spec.
- Locked mechanical/behavioural decisions (ascending ring, MMM:S0 countdown,
  one-way rotation, standalone TMC2209 at 1/16, ESP32-C5 only) are not up for
  re-litigation. Their rationale is in the spec's decision log and
  `docs/ref/README.md` §7 if present.
- When a `[Q]` gets answered or a default is overturned, append it to the
  decision log in `docs/FIRMWARE_SPEC.md` §17 with the reason.

## Hard technical constraints

- Rotation is one direction only. Every move is `(target − current) mod 50`
  flips forward. DIR is tied in hardware; there is no DIR GPIO.
- µsteps per spool revolution is **8242.42 — not an integer**. Positions are
  expressed relative to the most recent Hall edge using
  `T(i) = (2*i*5440 + 33) / 66`. Do not accumulate per-flap step counts.
- The pin map lives in one header (`components/swan_hal/include/hal/pins.h`)
  selected by a board define (`-DSWAN_BOARD=devkitc1|xiao`), so XIAO vs
  DevKitC-1 is a build flag, not a code change.
  On any C5 board, GPIO2/3/7/25/26/27/28 are strapping pins: only high-Z
  loads (I2S) go there — never a Hall, EN, or a shift-register output.
- The step ISR and all data it touches must be IRAM/DRAM-safe
  (`CONFIG_GPTIMER_ISR_IRAM_SAFE`, `IRAM_ATTR`, `DRAM_ATTR`). NVS/OTA writes
  disable the flash cache; a non-IRAM ISR drops steps.
- Hall inputs are sampled inside the step ISR, not via GPIO interrupts, so the
  edge latches the step count consistently.
- Single core. Keep the motion control tick (1 kHz) at a higher priority than
  networking; keep WiFi/MQTT/HTTP off the motion path entirely.
- The ring is **fluid**: it is a runtime file (`ring.json` in LittleFS,
  per-column capable) generated from `docs/ref/manifest.json` by
  `tools/ringgen.py`. No code references a ring index directly — look up by
  role (`digit(7)`, `blank`, `wifi`, name). Never type glyph names by hand.
- One command dispatcher (spec §10.2a). Web UI, MQTT, CLI, button and HA all
  feed it. MQTT is the canonical external API — a separate terminal prop will
  drive the display with it — so never add a control path that bypasses it.

## Toolchain

- ESP-IDF 5.5.x (latest patch), target `esp32c5`, C++17 for application code.
- Console on USB-Serial-JTAG.
- Host-side unit tests build with plain CMake (no IDF) under `test/host/`.

This is a Windows machine and ESP-IDF cannot run under Git Bash — `install.sh`
refuses with "MSys/Mingw is not supported". Activation is
`. $HOME\esp\esp-idf\export.ps1`, and the two wrapper scripts are the
documented commands; both must be run from PowerShell.

```
.\build.ps1 set-target esp32c5
.\build.ps1                       # idf.py build
.\build.ps1 menuconfig            # only for documented options
.\build.ps1 -p <PORT> flash monitor
.\test-host.ps1                   # host unit tests
```

`build.ps1` forwards any arguments to `idf.py` after sourcing ESP-IDF.
`test-host.ps1` uses the CMake and Ninja under `~/.espressif/tools` plus a
user-scope MinGW-w64 GCC. On a POSIX machine the plain forms apply instead:

```
idf.py set-target esp32c5 && idf.py build
cmake -S test/host -B build/host && cmake --build build/host && ctest --test-dir build/host
```

Pin the IDF version in `README.md` once the first build succeeds, and record
the board that was chosen and its chip revision there too.

## Repository layout (target)

```
CLAUDE.md
README.md                      build/flash instructions, pinned versions, pin map
docs/FIRMWARE_SPEC.md          the spec (source of truth for behaviour)
docs/BRINGUP.md                bench checklists + results as they come in
docs/MOTION_SYNC.md            motion ownership/atomics/critical-section contract
docs/ref/                      README.md (mechanical v6), BOM.md, manifest.json — supplied by Nico
.github/workflows/ci.yml       CI: host tests native, both boards in espressif/idf docker
build.ps1 / test-host.ps1      the documented build/test commands on this machine
main/                          app_main.cpp, task wiring
components/swan_hal/           pin map, GPIO bank writes, I2S init, LED (ESP-IDF owns the name `hal`)
components/motion/             axis_control.{h,cpp}: pure control core (host-tested);
                               motion.cpp: IDF shell (ISR, task, locks, GPIO)
components/ring/               ring table (generated), index math — host-testable
components/frame/              frame scheduler
components/modes/              clock, message, countdown — host-testable
components/timesvc/            SNTP, TZ
components/audio/              WAV player
components/net/                wifi, provisioning, mdns, httpd+ws, mqtt, ha discovery, ota
components/config/             NVS schema, defaults
components/cli/                console commands
web/                           static UI sources → LittleFS image
audio/                         WAV assets (Nico-supplied; placeholders generated)
tools/                         ring table generator, LittleFS packer, simulator
test/host/                     unit tests
```

## Git and GitHub — Claude manages both

Claude owns version control and the GitHub side; Nico does not hand-commit.
The rules, from 2026-08-21:

- **Commit in logical groups as you go** — `<component>: <what>` messages,
  one concern per commit, never a single end-of-task blob.
- **Push at the end of every task.** Remote: `siscan/lost-swan-firmware`,
  branch `master`.
- **Never force-push or rewrite history on `master`.** No `--force`, no
  `--force-with-lease`, no rebase/amend of anything already pushed. Fix
  forward with a new commit.
- **After every push, check the Actions result** (`gh run watch` /
  `gh run list`) and **fix failures before reporting** — a task is not done
  while CI is red. Linux CI is the reliability source of truth, so a local
  pass does not excuse a CI failure.
- Authentication is Nico's (`gh auth login`); never enter or handle tokens.

## Conventions

- Pure logic (ring, modes, schedules, position math) has no IDF includes so
  it compiles on the host. Hardware access goes through `hal/`.
- Every spec section that becomes code gets a unit test first where one is
  possible (ring math, `T(i)`, clock render, countdown schedule).
- Log with `ESP_LOGx` under per-component tags; never `printf` from tasks.
- Config keys in the spec §11 are the user-visible names; NVS keys are the
  ≤15-char short forms in the single mapping table. Nowhere else.
- Commit messages: `<component>: <what>` — e.g. `motion: latch hall edge in step ISR`.
- Don't add dependencies (managed components) without a one-line reason in
  `README.md`.
- Never name a project component after an ESP-IDF component (`hal`, `driver`,
  `log`, `nvs_flash`, `esp_timer`, …) — a same-named project component
  overrides the IDF one and breaks the build.
- When the repo's real layout diverges from the list above, fix the list
  here; this file must describe what exists.
- If the toolchain is missing, say so once and stop after scaffolding —
  do not keep writing uncompiled code across phases. Phase exit requires
  `idf.py build` and the host tests passing.

## Current phase

**Phase 1 accepted; Phase 1.5 (simulated-axis suite + CI) done. Next: Phase 2**
per spec §15 — frame + modes + simulator, noting the §7.3 countdown is a
deadline model, the §7.1 WiFi glyph sits on the centre column, and the ring
becomes the runtime `ring.json` file (spec §4) with the compiled table as
fallback. Motion changes must keep `docs/MOTION_SYNC.md` true and the
simulated-axis suite green; Linux CI is the reliability source of truth.
Nothing has run on hardware yet — the board has not arrived; the gear ratio
(85/33 vs the stale 68/26 prose) is settled by bench step 4, not by code.
