// The invariants an OTA could plausibly have broken (spec 10.4).
//
// Deliberately tiny and deliberately global: the confirm watcher has to be able
// to ask "did this boot actually work" without holding a reference to every
// subsystem, and every one of these is a single write from the code that knows.
//
// What is here is the whole criterion, and what is NOT here matters as much:
// homing, axis state, WiFi, SNTP, mDNS, MQTT and LittleFS are all excluded.
// "A successful boot + home" was the original wording and it bricks three ways
// - maintenance never homes and survives the reboot, a disabled column is never
// homed, and an unwired board always faults - each of them a ping-pong between
// the two slots. Gating on WiFi would mean "somebody changed the SSID" rolls
// you into an image that also cannot join.
#pragma once

#include <atomic>
#include <cstdint>

namespace swan {

inline std::atomic<bool> g_app_main_completed{false};
inline std::atomic<bool> g_config_init_ok{false};
inline std::atomic<bool> g_nvs_was_erased{false};
inline std::atomic<bool> g_httpd_started{false};
inline std::atomic<uint32_t> g_modes_ticks{0};

inline bool app_main_completed() { return g_app_main_completed.load(std::memory_order_relaxed); }
inline bool config_init_ok() { return g_config_init_ok.load(std::memory_order_relaxed); }
inline bool nvs_was_erased() { return g_nvs_was_erased.load(std::memory_order_relaxed); }
inline bool httpd_started() { return g_httpd_started.load(std::memory_order_relaxed); }
inline uint32_t modes_tick_count() { return g_modes_ticks.load(std::memory_order_relaxed); }

}  // namespace swan
