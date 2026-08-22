#include "ring/ring_store.h"

#include <cstdio>
#include <string>

#include "esp_littlefs.h"
#include "esp_log.h"

namespace swan {
namespace ring_store {
namespace {

constexpr const char* TAG = "ring";
constexpr const char* BASE = "/fs";
constexpr const char* RING_PATH = "/fs/ring.json";

RingSet g_set = RingSet::compiled_fallback();
bool g_mounted = false;

esp_err_t read_file(const char* path, std::string& out) {
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return ESP_ERR_NOT_FOUND;
    std::fseek(f, 0, SEEK_END);
    const long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (len < 0 || len > 256 * 1024) {
        std::fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    out.resize(static_cast<size_t>(len));
    const size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    return got == out.size() ? ESP_OK : ESP_FAIL;
}

}  // namespace

esp_err_t init() {
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = BASE;
    conf.partition_label = "storage";
    conf.format_if_mount_failed = true;  // a virgin partition is expected
    conf.dont_mount = false;

    const esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "littlefs mount failed (%s); compiled ring table active",
                 esp_err_to_name(err));
        return ESP_OK;  // never fail the boot over the filesystem
    }
    g_mounted = true;

    size_t total = 0, used = 0;
    if (esp_littlefs_info("storage", &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "littlefs mounted: %u/%u KB used", static_cast<unsigned>(used / 1024),
                 static_cast<unsigned>(total / 1024));
    }
    reload();
    return ESP_OK;
}

esp_err_t reload() {
    if (!g_mounted) return ESP_ERR_INVALID_STATE;

    std::string text;
    esp_err_t err = read_file(RING_PATH, text);
    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "no %s; compiled ring table active", RING_PATH);
        g_set = RingSet::compiled_fallback();
        return err;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reading %s failed (%s); compiled ring table active", RING_PATH,
                 esp_err_to_name(err));
        g_set = RingSet::compiled_fallback();
        return err;
    }

    std::string why;
    if (!g_set.load_json(text, &why)) {
        // load_json already reset to the fallback.
        ESP_LOGE(TAG, "%s rejected (%s); compiled ring table active", RING_PATH, why.c_str());
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "ring table loaded from %s (%d slots)", RING_PATH,
             g_set.col(0).slot_count());
    return ESP_OK;
}

const RingSet& get() { return g_set; }

}  // namespace ring_store
}  // namespace swan
