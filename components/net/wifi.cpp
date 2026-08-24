#include "net/provision.h"
#include "net/wifi.h"

#include <cstring>
#include <mutex>

#include "config/config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs_flash.h"

namespace swan {
namespace net {
namespace {

constexpr const char* TAG = "wifi";

// Reconnect backoff: a wall clock that loses its AP must not spin the radio.
// The display keeps free-running time meanwhile (spec 8), so there is no hurry.
constexpr int kBackoffMs[] = {1000, 2000, 5000, 10000, 30000};

std::mutex g_mu;
WifiStatus g_status;
esp_netif_t* g_netif = nullptr;
bool g_started = false;
int g_attempt = 0;
esp_timer_handle_t g_retry = nullptr;

void set_state(WifiState s) {
    const std::lock_guard<std::mutex> lock(g_mu);
    g_status.state = s;
}

// Told on every link transition.  A single slot, set once at boot before the
// WiFi task exists, so no lock: adding a second consumer means a small array
// and a mutex, not a second global.
LinkCallback g_link_cb = nullptr;
bool g_link_up = false;

void notify_link(bool up) {
    if (g_link_up == up) return;   // only transitions, never a repeat
    g_link_up = up;
    if (g_link_cb != nullptr) g_link_cb(up);
}

void schedule_retry() {
    const int idx = g_attempt < static_cast<int>(sizeof kBackoffMs / sizeof kBackoffMs[0])
                        ? g_attempt
                        : static_cast<int>(sizeof kBackoffMs / sizeof kBackoffMs[0]) - 1;
    ++g_attempt;
    esp_timer_stop(g_retry);
    ESP_ERROR_CHECK(esp_timer_start_once(g_retry, static_cast<uint64_t>(kBackoffMs[idx]) * 1000));
    ESP_LOGW(TAG, "reconnecting in %d ms (attempt %d)", kBackoffMs[idx], g_attempt);
}

void retry_cb(void*) {
    if (g_started) esp_wifi_connect();
}

void on_wifi_event(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base != WIFI_EVENT) return;
    switch (id) {
        case WIFI_EVENT_STA_START:
            set_state(WifiState::Connecting);
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            {
                const std::lock_guard<std::mutex> lock(g_mu);
                ++g_status.disconnects;
                g_status.ip.clear();
                g_status.rssi = 0;
                g_status.state = WifiState::Failed;
            }
            notify_link(false);
            const auto* e = static_cast<wifi_event_sta_disconnected_t*>(data);
            ESP_LOGW(TAG, "disconnected, reason %d", e != nullptr ? e->reason : -1);
            schedule_retry();
            break;
        }
        default:
            break;
    }
}

void on_got_ip(void*, esp_event_base_t, int32_t, void* data) {
    const auto* e = static_cast<ip_event_got_ip_t*>(data);
    char buf[16];
    std::snprintf(buf, sizeof buf, IPSTR, IP2STR(&e->ip_info.ip));
    {
        const std::lock_guard<std::mutex> lock(g_mu);
        g_status.state = WifiState::Connected;
        g_status.ip = buf;
    }
    g_attempt = 0;
    // The portal's job is done the moment there is a route.  Direct rather
    // than through on_link_change, which is a single slot MQTT already owns.
    provision_stop_on_join();
    notify_link(true);
    ESP_LOGI(TAG, "connected, ip %s", buf);
}

esp_err_t start_sta(const config::WifiConfig& cfg) {
    wifi_config_t wc = {};
    std::strncpy(reinterpret_cast<char*>(wc.sta.ssid), cfg.ssid.c_str(),
                 sizeof wc.sta.ssid - 1);
    std::strncpy(reinterpret_cast<char*>(wc.sta.password), cfg.pass.c_str(),
                 sizeof wc.sta.password - 1);
    // Dual band follows the SSID (spec 10.1); an open AP is allowed because
    // some props live on one.
    wc.sta.threshold.authmode = cfg.pass.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    const esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) return err;
    g_started = true;
    {
        const std::lock_guard<std::mutex> lock(g_mu);
        g_status.ssid = cfg.ssid;
        g_status.state = WifiState::Connecting;
    }
    return ESP_OK;
}

}  // namespace

const char* wifi_state_name(WifiState s) {
    switch (s) {
        case WifiState::Disabled:   return "disabled";
        case WifiState::Connecting: return "connecting";
        case WifiState::Connected:  return "connected";
        case WifiState::Failed:     return "failed";
    }
    return "?";
}

esp_err_t init() {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    g_netif = esp_netif_create_default_wifi_sta();

    const wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &on_wifi_event, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &on_got_ip, nullptr, nullptr));

    const esp_timer_create_args_t targs = {
        .callback = &retry_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_retry",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &g_retry));

    config::WifiConfig cfg;
    ESP_ERROR_CHECK(config::load_wifi(cfg));
    if (!cfg.configured()) {
        // Standalone is a supported state, not an error (spec 10.0): the
        // display runs, SNTP never syncs, and the centre column shows the
        // WiFi glyph after the grace period.
        ESP_LOGW(TAG, "no credentials stored - run: wifi <ssid> <password>");
        set_state(WifiState::Disabled);
        return ESP_OK;
    }
    ESP_LOGI(TAG, "connecting to '%s'", cfg.ssid.c_str());
    return start_sta(cfg);
}

esp_err_t set_credentials(const std::string& ssid, const std::string& pass) {
    config::WifiConfig cfg{ssid, pass};
    const esp_err_t err = config::save_wifi(cfg);
    if (err != ESP_OK) return err;

    g_attempt = 0;
    esp_timer_stop(g_retry);
    // esp_wifi_stop() takes the ACCESS POINT down with it, and this is called
    // from the dispatcher while a phone is connected to that access point
    // through the portal - so the client never received the answer to its own
    // request, and if the password was wrong it had no way back in.  The AP now
    // stays up until the STA actually joins (provision_stop_on_join), which is
    // the only evidence that the credentials were right.
    if (g_started) {
        esp_wifi_disconnect();
        g_started = false;
    }
    if (!cfg.configured()) {
        const std::lock_guard<std::mutex> lock(g_mu);
        g_status = WifiStatus{};
        return ESP_OK;
    }
    return start_sta(cfg);
}

void on_link_change(LinkCallback cb) { g_link_cb = cb; }

WifiStatus status() {
    WifiStatus out;
    {
        const std::lock_guard<std::mutex> lock(g_mu);
        out = g_status;
    }
    if (out.state == WifiState::Connected) {
        wifi_ap_record_t rec;
        if (esp_wifi_sta_get_ap_info(&rec) == ESP_OK) out.rssi = rec.rssi;
    }
    return out;
}

esp_err_t mdns_start(const char* hostname, const char* instance) {
    esp_err_t err = mdns_init();
    if (err != ESP_OK) return err;
    ESP_ERROR_CHECK(mdns_hostname_set(hostname));
    ESP_ERROR_CHECK(mdns_instance_name_set(instance));
    ESP_ERROR_CHECK(mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0));
    ESP_LOGI(TAG, "mdns: http://%s.local/", hostname);
    return ESP_OK;
}

}  // namespace net
}  // namespace swan
