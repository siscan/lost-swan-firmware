# lost-swan-firmware

Firmware for the LOST Swan-station split-flap display.
Target: ESP32-C5-DevKitC-1-N8R8 (XIAO ESP32-C5 map behind a board define).

**Start here:** `CLAUDE.md` (working agreement) → `docs/FIRMWARE_SPEC.md` (the spec).

## Status

- **Spec v1.0** — all questions answered (spec §16); resolutions in the §17 decision log
- Hardware: DevKitC-1 on order; **chip revision unverified** (checked on first flash)
- Code: **Phase 1 complete; all exit criteria met.** Nothing has been flashed or
  measured on hardware — `docs/BRINGUP.md` tracks that separately.

| Phase 1 exit criterion | status |
|---|---|
| `set-target esp32c5` + `build` clean | passes — 304 KB, 88% of the app partition free, zero warnings |
| XIAO board map builds | passes — 282 KB |
| host tests green | 2/2 pass |
| `git diff` empty after `gen_ring_table.py` | clean — regeneration is byte-identical |
| motion cross-task handoff explicit | done — spinlock + request mailbox + atomics, no store-ordering |
| flashed to hardware | **not done — board not arrived** |

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
.\test-host.ps1            # configure, build, run
.\test-host.ps1 -Clean     # wipe build/host first
```

It uses the CMake and Ninja that `install.ps1` already put under
`~/.espressif/tools` — no separate CMake install — plus the user-scope MinGW-w64
GCC from winget. No Visual Studio, nothing needing admin.

Two machine constraints are baked into that script and into
`test/host/CMakeLists.txt`:

- An **Application Control policy blocks `ar.exe`** (and WinLibs' own bundled
  `cmake.exe`). So the suite compiles the pure sources straight into each test
  executable rather than building a static library, and uses the **Ninja**
  generator — MinGW Makefiles archives objects with `ar` before linking and can
  never work here.
- The winget tools are not on the inherited PATH, so the script locates them.

On a machine without those constraints the plain form in CLAUDE.md works as-is:

```bash
cmake -S test/host -B build/host && cmake --build build/host && ctest --test-dir build/host --output-on-failure
```

## Ring table

Regenerate after any change to `docs/ref/manifest.json`:

```powershell
python tools/gen_ring_table.py
```

`python tools/gen_ring_table.py --check` fails if the committed header is stale;
run it in CI.

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

Nothing else. The host tests deliberately have no test framework — three macros
in `test/host/check.h` cover what they need.

## How motion synchronises across tasks

Three boundaries, one explicit mechanism each — no store-ordering convention
(CLAUDE.md Phase 1 exit criterion). All of it is in `components/motion/motion.cpp`:

| boundary | mechanism |
|---|---|
| step ISR ↔ control task | `portMUX` spinlock over a DRAM-only `AxisIsr` struct. The ISR cannot take a lock, so the task side takes it — that disables interrupts and excludes the ISR. |
| any task → control task | per-axis **request mailbox**, written and drained under the same spinlock. The control task is the only writer of the FSM, so a command can never race a state transition. |
| control task → any task | `std::atomic` with explicit `memory_order_relaxed`, for published state and diagnostics. Anything needing a consistent multi-field view (position vs target vs velocity) goes through the spinlock instead. |

`AxisIsr` is a separate struct from `Axis` on purpose, so the IRAM/DRAM boundary
is visible in the type system rather than asserted in a comment.

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
components/config/     NVS schema and defaults
components/cli/        bring-up console (spec §13)
main/                  boot sequence
tools/                 ring table generator
test/host/             host unit tests
```

`components/ring/` and `components/motion/include/motion/motion_math.h` are pure
— no IDF includes — so the host tests compile them directly.
