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
    wifi_mode_t prev = WIFI_MODE_NULL;
    esp_wifi_get_mode(&prev);
    const wifi_mode_t want = prev == WIFI_MODE_STA ? WIFI_MODE_APSTA : WIFI_MODE_AP;
    esp_err_t err = esp_wifi_set_mode(want);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "portal: set_mode failed: %s", esp_err_to_name(err));
        return err;
    }

    // ONE radio, so the access point cannot pick its own channel while the
    // station is associated: it has to use the station's.  Hard-coding 1 was a
    // remote PANIC - with the STA on a 5 GHz channel esp_wifi_set_config
    // returns ESP_ERR_INVALID_ARG ("channel number 1 is out of supported 5G
    // channel range of AP") and the ESP_ERROR_CHECK that used to wrap it
    // aborted the board.  Any `wifi.provision` on a 5 GHz network rebooted the
    // display, which is the one command whose whole job is to rescue a display
    // that cannot reach the network.
    uint8_t channel = 1;   // unassociated: 2.4 GHz, the most findable by a phone
    wifi_ap_record_t joined = {};
    if (esp_wifi_sta_get_ap_info(&joined) == ESP_OK && joined.primary != 0) {
        channel = joined.primary;
        ESP_LOGI(TAG, "portal follows the station onto channel %u", channel);
    }

    wifi_config_t cfg = {};
    std::strncpy(reinterpret_cast<char*>(cfg.ap.ssid), ssid.c_str(), sizeof cfg.ap.ssid - 1);
    cfg.ap.ssid_len = static_cast<uint8_t>(ssid.size());
    cfg.ap.channel = channel;
    cfg.ap.max_connection = 4;
    // OPEN, deliberately.  A password would have to be printed on the case or
    // guessed, and the portal exists precisely for the person standing in front
    // of a display that cannot reach the network.  The exposure is: someone in
    // radio range can join and set the WiFi credentials.  They could also
    // unplug it.
    cfg.ap.authmode = WIFI_AUTH_OPEN;
    err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "portal: ap config (channel %u) failed: %s", channel, esp_err_to_name(err));
        esp_wifi_set_mode(prev);        // leave the radio as we found it
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "portal: wifi start failed: %s", esp_err_to_name(err));
        esp_wifi_set_mode(prev);
        return err;
    }

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
    // Unconditionally STA, not only from APSTA.  The old test left an OPEN
    // access point beaconing for ever whenever the portal had been started in
    // AP-only mode - which is the case when there was no STA to coexist with,
    // i.e. exactly the unprovisioned boot the portal exists for.
    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "provisioning stopped; access point down");
    return ESP_OK;
}

// Called from the link-up path: once the display is actually on a network, the
// portal has done its job.  Leaving it up would keep an open AP beaconing and
// keep a DNS hijack answering every query with 192.168.4.1 - following the
// display onto the LAN, where anything that asked it for a name would be sent
// to an address that is not there.
void provision_stop_on_join() {
    if (!g_active.load(std::memory_order_relaxed)) return;
    ESP_LOGI(TAG, "joined a network; taking the portal down");
    provision_stop();
}

bool provisioning() { return g_active.load(std::memory_order_relaxed); }
bool portal_active() { return g_active.load(std::memory_order_relaxed); }

std::string provision_ssid() {
    const std::lock_guard<std::mutex> lock(g_mu);
    return g_ssid;
}

}  // namespace net
}  // namespace swan
