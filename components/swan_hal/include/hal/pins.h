// The pin map.  One header, selected by a board define, so XIAO vs DevKitC-1 is
// a build flag and not a code change (CLAUDE.md hard constraints).
//
// Board select:  cmake -DSWAN_BOARD=xiao ...   (default: devkitc1)
// See README.md for the resolved map of whichever board is built.
#pragma once

#include <cstdint>

#if !defined(BOARD_ESP32C5_DEVKITC1) && !defined(BOARD_XIAO_ESP32C5)
#define BOARD_ESP32C5_DEVKITC1 1
#endif
#if defined(BOARD_ESP32C5_DEVKITC1) && defined(BOARD_XIAO_ESP32C5)
#error "Select exactly one board: BOARD_ESP32C5_DEVKITC1 or BOARD_XIAO_ESP32C5"
#endif

namespace swan {

inline constexpr int N_COLUMNS = 5;

// ---------------------------------------------------------------------------
// ESP32-C5 strapping pins, per Espressif's DevKitC-1 v1.2 user guide (spec 2.0).
// Rule on any C5 board: only high-impedance loads (the amp's I2S inputs) go
// here.  Never a Hall (it carries a 10k pull-up), never EN (the driver modules
// carry pull-ups), never a shift-register output.  Enforced below.
// ---------------------------------------------------------------------------
constexpr bool is_strapping_pin(int gpio) {
    return gpio == 2 || gpio == 3 || gpio == 7 || gpio == 25 || gpio == 26 || gpio == 27 ||
           gpio == 28;
}

#if defined(BOARD_ESP32C5_DEVKITC1)

// Spec 2.2 - everything on headers, nothing driven on a strapping pin.
inline constexpr const char* BOARD_NAME = "ESP32-C5-DevKitC-1-N8R8";
inline constexpr int PIN_STEP[N_COLUMNS] = {8, 9, 10, 11, 12};
inline constexpr int PIN_HALL[N_COLUMNS] = {0, 1, 4, 5, 23};
inline constexpr int PIN_EN = 6;  // ganged, active low
inline constexpr int PIN_DIR = 24;  // ganged; the one spare non-strapping pin
inline constexpr int PIN_I2S_BCLK = 7;
inline constexpr int PIN_I2S_LRCLK = 25;
inline constexpr int PIN_I2S_DIN = 26;
inline constexpr int PIN_BUTTON = 28;  // onboard BOOT; external button in parallel
inline constexpr int PIN_LED = 27;     // WS2812 RGB
inline constexpr bool LED_IS_RGB = true;
inline constexpr bool LED_ACTIVE_LOW = false;

#else  // BOARD_XIAO_ESP32C5

// Spec 2.1 - needs four back-pad solder joints (GPIO5, GPIO4, GPIO3, BOOT).
inline constexpr const char* BOARD_NAME = "Seeed XIAO ESP32-C5";
inline constexpr int PIN_STEP[N_COLUMNS] = {11, 12, 8, 9, 10};
inline constexpr int PIN_HALL[N_COLUMNS] = {0, 23, 24, 5, 4};
inline constexpr int PIN_EN = 1;  // ganged, active low
// NO DIR GPIO ON THIS BOARD.  The XIAO breaks out exactly eleven
// non-strapping pins and all eleven are spoken for (5 STEP + EN + 5 HALL);
// the only free pins left are GPIO2 and GPIO26, both strapping.  Driving a
// strapping pin is the one thing the pin rules forbid outright, and a
// TMC2209 module's pull on DIR would hold the strap at reset - so DIR stays
// tied at the drivers here and `motion.dir_invert` is refused, with a
// reason, rather than silently doing nothing (spec 2.1).
inline constexpr int PIN_DIR = -1;
inline constexpr int PIN_I2S_BCLK = 7;
inline constexpr int PIN_I2S_LRCLK = 25;
inline constexpr int PIN_I2S_DIN = 3;
inline constexpr int PIN_BUTTON = 28;
inline constexpr int PIN_LED = 27;  // single yellow LED
inline constexpr bool LED_IS_RGB = false;
inline constexpr bool LED_ACTIVE_LOW = true;

#endif

// DIR IS A GPIO AGAIN, on boards that have a pin for it (spec 2.2, 2026-09-06).
//
// It was deliberately absent, and that was right for the bridge design: the
// motor hung outside the drum, the drum's sense was fixed by the gear train,
// and reverse was mechanically forbidden, so a DIR GPIO bought nothing and
// cost a pin the XIAO could not spare.  The motor now sits INSIDE the drum
// and faces the other way, which inverts the relationship between motor sense
// and drum sense - and the ring is descending, so getting that backwards
// makes every countdown count up.  Deciding it in firmware beats discovering
// it with a soldering iron.
//
// GANGED, like EN: one pin, five drivers, one global `motion.dir_invert`.  A
// single column that needs the opposite sense is still a wiring change - the
// pins for five separate DIR lines do not exist on either board.
//
// `HAS_DIR_GPIO` is false where no pin was available; every caller must cope
// with that rather than assume.
inline constexpr bool HAS_DIR_GPIO = PIN_DIR >= 0;

// ---------------------------------------------------------------------------
// Compile-time checks.  These are the constraints that are expensive to get
// wrong on a soldered board, so they fail the build instead of the bench.
// ---------------------------------------------------------------------------
namespace detail {

constexpr bool none_strapping(const int (&a)[N_COLUMNS]) {
    for (int i = 0; i < N_COLUMNS; ++i) {
        if (is_strapping_pin(a[i])) return false;
    }
    return true;
}

constexpr bool all_below_32(const int (&a)[N_COLUMNS]) {
    for (int i = 0; i < N_COLUMNS; ++i) {
        if (a[i] < 0 || a[i] > 31) return false;
    }
    return true;
}

// Every driven signal, for the uniqueness check.
inline constexpr int ALL_PINS[] = {
    PIN_STEP[0], PIN_STEP[1], PIN_STEP[2], PIN_STEP[3], PIN_STEP[4],
    PIN_HALL[0], PIN_HALL[1], PIN_HALL[2], PIN_HALL[3], PIN_HALL[4],
    PIN_EN,      PIN_I2S_BCLK, PIN_I2S_LRCLK, PIN_I2S_DIN, PIN_BUTTON, PIN_LED,
    // -1 where the board has no DIR pin; the uniqueness check below ignores
    // negatives, so an absent signal cannot collide with a present one.
    PIN_DIR,
};
inline constexpr int ALL_PINS_N = sizeof(ALL_PINS) / sizeof(ALL_PINS[0]);

constexpr bool pins_unique() {
    for (int i = 0; i < ALL_PINS_N; ++i) {
        if (ALL_PINS[i] < 0) continue;   // an absent signal
        for (int j = i + 1; j < ALL_PINS_N; ++j) {
            if (ALL_PINS[i] == ALL_PINS[j]) return false;
        }
    }
    return true;
}

}  // namespace detail

static_assert(detail::none_strapping(PIN_STEP),
              "a STEP pin landed on a C5 strapping pin - see spec 2.0");
static_assert(detail::none_strapping(PIN_HALL),
              "a HALL pin landed on a C5 strapping pin; its 10k pull-up would "
              "hold the strap and change the boot mode - see spec 2.0");
static_assert(!is_strapping_pin(PIN_EN),
              "EN landed on a C5 strapping pin; the driver module's pull-up "
              "would hold the strap - see spec 2.0");
static_assert(!HAS_DIR_GPIO || !is_strapping_pin(PIN_DIR),
              "DIR landed on a C5 strapping pin; a TMC2209 module's pull on DIR "
              "would hold the strap at reset - see spec 2.0");
static_assert(detail::all_below_32(PIN_STEP) && detail::all_below_32(PIN_HALL) && PIN_EN < 32,
              "single-bank GPIO writes require every motion pin below GPIO32");
static_assert(!HAS_DIR_GPIO || PIN_DIR < 32,
              "single-bank GPIO writes require every motion pin below GPIO32");
static_assert(detail::pins_unique(), "two signals are assigned the same GPIO");

}  // namespace swan
