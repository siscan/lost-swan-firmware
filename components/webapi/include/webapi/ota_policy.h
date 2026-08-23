// OTA policy (spec 10.4).  Pure - host-tested, no IDF.
//
// Three decisions live here, and all three are the kind that are cheap to get
// right in a test and expensive to get wrong on a wall:
//
//   1. Is this image even for this device?  ESP-IDF checks the magic, the
//      SHA256, the chip id and the chip revision, and every one of those
//      PASSES for an image built with the other board's pin map - which then
//      drives STEP on the wrong GPIOs.  Only the project name and the version
//      tag can tell those apart, and only if somebody looks.
//
//   2. May an OTA start right now?  Not while a drum is turning.
//
//   3. Has the new image earned the right to keep running?  Spec 10.4 used to
//      say "a successful boot + home", which cannot be satisfied in
//      maintenance, cannot be satisfied with a disabled column, and is not
//      satisfied by an unwired board - three different ways to boot-loop
//      between the two slots.  The criterion is now local invariants an OTA
//      could plausibly have broken.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace swan {
namespace api {

// esp_image_header_t (24 B) + esp_image_segment_header_t (8 B) + esp_app_desc_t
// (256 B).  The sniffer needs exactly this much of the stream before it can say
// anything, which is why the upload handler buffers the first chunk.
inline constexpr std::size_t OTA_HEADER_BYTES = 288;

// ESP_CHIP_ID_ESP32C5, from esp_app_format.h.  Hard-coded rather than included
// so this file stays host-compilable; the host test asserts it against a real
// built binary, which is the only check that means anything.
inline constexpr uint16_t OTA_CHIP_ID_ESP32C5 = 0x0017;

struct ImageInfo {
    bool parsed = false;
    uint16_t chip_id = 0xFFFF;
    uint16_t min_chip_rev_full = 0;
    uint16_t max_chip_rev_full = 0;
    std::string project_name;
    std::string version;       // PROJECT_VER, e.g. "0.4.0+devkitc1.sim"
    std::string idf_ver;
    // Parsed out of the version tag, which is where the board map and the
    // sim/release flavour live (nothing in esp_app_desc_t distinguishes them).
    std::string board;         // "devkitc1" | "xiao" | "" if untagged
    std::string flavour;       // "sim" | "rel" | "nosim" | ""
};

// Read what the first OTA_HEADER_BYTES of an image say about itself.
ImageInfo sniff_image(const uint8_t* data, std::size_t len);

enum class OtaVerdict : uint8_t {
    Allow,
    BadImage,        // not an ESP-IDF app image at all
    WrongChip,
    WrongProject,    // a different firmware entirely
    WrongBoard,      // the other pin map: passes every check IDF makes
    LosesSimulation, // a release image onto a board saved all-simulated
    Moving,          // a drum is turning
    PendingVerify,   // the running image has not confirmed itself yet
    NoRoom,
};
const char* ota_verdict_name(OtaVerdict v);
// Human-readable, for the Update page and swan/event.  Never a bare code.
const char* ota_verdict_reason(OtaVerdict v);

struct OtaPrecheck {
    ImageInfo image;
    std::string running_project;
    std::string running_board;
    bool all_axes_idle = true;
    bool maintenance = false;
    bool running_pending_verify = false;
    bool any_simulated_column = false;
    uint32_t free_heap = 0;
    bool force = false;         // the user insisted; overrides the soft refusals
};

OtaVerdict ota_gate(const OtaPrecheck& p);

// ---------------------------------------------------------------------------
// Boot health (spec 10.4)
// ---------------------------------------------------------------------------
// What the running image must demonstrate before it marks itself valid.
//
// Deliberately EXCLUDED, each for a reason that has already bitten:
//   homing / axis state - maintenance never homes and survives the reboot; a
//                         disabled column is never homed; an unwired board
//                         always faults.  Any of the three would boot-loop.
//   WiFi / SNTP / mDNS  - "somebody changed the SSID" would roll you into an
//                         image that also cannot join.
//   MQTT / LittleFS     - optional by design (spec 10.0).
struct BootHealth {
    bool app_main_completed = false;
    bool config_ok = false;        // config::init() returned OK ...
    bool nvs_was_erased = false;   // ... and did NOT erase the partition
    bool httpd_ok = false;
    uint32_t modes_ticks = 0;
    uint32_t motion_ticks = 0;
    uint32_t uptime_s = 0;
};

inline constexpr uint32_t OTA_MIN_MODES_TICKS = 200;    // 10 s at 20 Hz
inline constexpr uint32_t OTA_MIN_MOTION_TICKS = 10000; // 10 s at 1 kHz
inline constexpr uint32_t OTA_CONFIRM_DEADLINE_S = 120;

enum class BootVerdict : uint8_t { Wait, Confirm, Rollback };
const char* boot_verdict_name(BootVerdict v);

BootVerdict ota_evaluate(const BootHealth& h);

}  // namespace api
}  // namespace swan
