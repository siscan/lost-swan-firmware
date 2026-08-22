#include "timesvc/time_service.h"

#include <atomic>
#include <ctime>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"

namespace swan {
namespace time_service {
namespace {

constexpr const char* TAG = "timesvc";

std::atomic<bool> g_valid{false};
bool g_started = false;

class SystemTime final : public TimeSource {
public:
    int64_t now_utc() override { return static_cast<int64_t>(std::time(nullptr)); }
    bool valid() override { return g_valid.load(std::memory_order_relaxed); }
};

SystemTime g_source;

}  // namespace

esp_err_t init(const char* ntp_server) {
    if (g_started) return ESP_OK;

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(ntp_server);
    cfg.start = true;
    cfg.sync_cb = [](timeval*) {
        // Sticky: a later WiFi drop free-runs, it does not invalidate (spec 8).
        g_valid.store(true, std::memory_order_relaxed);
        ESP_LOGI(TAG, "SNTP synced");
    };

    const esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sntp init failed: %s", esp_err_to_name(err));
        return err;
    }
    g_started = true;
    ESP_LOGI(TAG, "SNTP started (%s); time invalid until first sync", ntp_server);
    return ESP_OK;
}

TimeSource& source() { return g_source; }

}  // namespace time_service
}  // namespace swan
