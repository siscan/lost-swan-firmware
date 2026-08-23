#include "net/provision.h"

#include <atomic>
#include <cstring>
#include <mutex>

#include <lwip/sockets.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "webapi/portal.h"

namespace swan {
namespace net {
namespace {

constexpr const char* TAG = "portal";
constexpr uint32_t AP_IP = 0x0104A8C0;   // 192.168.4.1, network byte order

std::atomic<bool> g_active{false};
std::atomic<bool> g_dns_stop{false};
TaskHandle_t g_dns_task = nullptr;
esp_netif_t* g_ap_netif = nullptr;
std::mutex g_mu;
std::string g_ssid;

// A DNS server in ~40 lines, because the alternative is a managed component for
// something this small (CLAUDE.md: no dependency without a reason, and "we
// needed 40 lines of UDP" is not one).
void dns_task(void*) {
    const int sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "dns socket: %d", errno);
        g_dns_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        ESP_LOGE(TAG, "dns bind: %d", errno);
        ::close(sock);
        g_dns_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    timeval tv = {};
    tv.tv_sec = 1;   // so the stop flag is noticed promptly
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ESP_LOGI(TAG, "dns responder on :53");

    uint8_t in[512];
    uint8_t out[512];
    while (!g_dns_stop.load(std::memory_order_relaxed)) {
        sockaddr_in from = {};
        socklen_t flen = sizeof from;
        const int n = ::recvfrom(sock, in, sizeof in, 0, reinterpret_cast<sockaddr*>(&from),
                                 &flen);
        if (n <= 0) continue;
        const std::size_t r = api::dns_hijack_reply(in, static_cast<std::size_t>(n), AP_IP, out,
                                                    sizeof out);
        // Zero means "do not answer" - a malformed packet, a response being
        // reflected back at us, or a query type an A record would answer
        // wrongly.  Silence is the correct reply to all three.
        if (r == 0) continue;
        ::sendto(sock, out, r, 0, reinterpret_cast<sockaddr*>(&from), flen);
    }
    ::close(sock);
    g_dns_task = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace

esp_err_t provision_start() {
    if (g_active.load(std::memory_order_relaxed)) return ESP_OK;

    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    const std::string ssid = api::portal_ssid(mac);
    {
        const std::lock_guard<std::mutex> lock(g_mu);
        g_ssid = ssid;
    }

    if (g_ap_netif == nullptr) g_ap_netif = esp_netif_create_default_wifi_ap();

    // APSTA, not AP-only: the STA half keeps trying in the background, so a
    // display that came up unprovisioned because the router was down joins by
    // itself the moment it returns - without anyone having to notice.
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    ESP_ERROR_CHECK(esp_wifi_set_mode(mode == WIFI_MODE_STA ? WIFI_MODE_APSTA : WIFI_MODE_AP));

    wifi_config_t cfg = {};
    std::strncpy(reinterpret_cast<char*>(cfg.ap.ssid), ssid.c_str(), sizeof cfg.ap.ssid - 1);
    cfg.ap.ssid_len = static_cast<uint8_t>(ssid.size());
    cfg.ap.channel = 1;
    cfg.ap.max_connection = 4;
    // OPEN, deliberately.  A password would have to be printed on the case or
    // guessed, and the portal exists precisely for the person standing in front
    // of a display that cannot reach the network.  The exposure is: someone in
    // radio range can join and set the WiFi credentials.  They could also
    // unplug it.
    cfg.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    g_dns_stop.store(false, std::memory_order_relaxed);
    if (g_dns_task == nullptr) {
        xTaskCreate(&dns_task, "swan_dns", 3072, nullptr, 3, &g_dns_task);
    }
    g_active.store(true, std::memory_order_relaxed);
    ESP_LOGW(TAG, "*** PROVISIONING: join the open network \"%s\" and a sign-in page should "
                  "appear; otherwise browse to http://192.168.4.1/ ***", ssid.c_str());
    return ESP_OK;
}

esp_err_t provision_stop() {
    if (!g_active.load(std::memory_order_relaxed)) return ESP_OK;
    g_dns_stop.store(true, std::memory_order_relaxed);
    g_active.store(false, std::memory_order_relaxed);
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_APSTA) esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "provisioning stopped");
    return ESP_OK;
}

bool provisioning() { return g_active.load(std::memory_order_relaxed); }
bool portal_active() { return g_active.load(std::memory_order_relaxed); }

std::string provision_ssid() {
    const std::lock_guard<std::mutex> lock(g_mu);
    return g_ssid;
}

}  // namespace net
}  // namespace swan
