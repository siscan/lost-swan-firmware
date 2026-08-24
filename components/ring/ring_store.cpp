#include "ring/ring_store.h"

#include <cstdio>
#include <string>

#include "esp_littlefs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_task_wdt.h"

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

    // Mounting can FORMAT, and formatting the 2 MB storage partition is ~512
    // sector erases with the flash cache disabled: 10-25 s in which no task
    // runs at all, the idle task included.  With CONFIG_ESP_TASK_WDT_PANIC on
    // that would panic, and it would panic again on the next boot, and the
    // next - a virgin or corrupted board would never come up.
    //
    // So the idle task is unsubscribed across exactly this call and resubscribed
    // after.  Nothing is watched for the duration, which is correct: this is a
    // known-long operation, not a hang.
    TaskHandle_t idle = xTaskGetIdleTaskHandleForCPU(0);   // single core
    const bool unwatched = idle != nullptr && esp_task_wdt_delete(idle) == ESP_OK;

    const esp_err_t err = esp_vfs_littlefs_register(&conf);

    if (unwatched) {
        // Checked: the idle subscription is the ONLY coverage for "something
        // above priority 0 is spinning and starving everything below it", so
        // failing to restore it would silently remove a whole class of
        // detection - and it would be restored-looking in every other way.
        const esp_err_t re = esp_task_wdt_add(idle);
        if (re != ESP_OK) {
            ESP_LOGE(TAG, "*** the idle task is no longer watched (%s) - a spinning task will "
                          "no longer be caught ***", esp_err_to_name(re));
        }
    }
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

RingSet& mutable_ring() { return g_set; }

esp_err_t write_accepted(const std::string& body) {
    if (!g_mounted) return ESP_ERR_INVALID_STATE;

    // Temp file then rename: a power cut mid-write must not leave a truncated
    // ring.json that the next boot would reject.
    constexpr const char* TMP_PATH = "/fs/ring.json.new";
    std::FILE* f = std::fopen(TMP_PATH, "wb");
    if (f == nullptr) {
        ESP_LOGE(TAG, "cannot open %s for write", TMP_PATH);
        return ESP_FAIL;
    }
    const size_t wrote = std::fwrite(body.data(), 1, body.size(), f);
    const bool flushed = (std::fflush(f) == 0);
    std::fclose(f);
    if (wrote != body.size() || !flushed) {
        ESP_LOGE(TAG, "short write to %s (%u/%u bytes)", TMP_PATH,
                 static_cast<unsigned>(wrote), static_cast<unsigned>(body.size()));
        std::remove(TMP_PATH);
        return ESP_FAIL;
    }
    // Rename straight over the old file.  The previous code removed it first,
    // on the belief that LittleFS cannot rename onto an existing name - it
    // can: lfs_rename replaces a same-type file in a single directory commit,
    // which is exactly why this pattern was chosen.  Removing first opened a
    // window where a brownout (five columns spinning is the normal load) left
    // NO ring.json at all and the next boot silently fell back to the compiled
    // table, with the good upload sitting unread in ring.json.new.
    if (std::rename(TMP_PATH, RING_PATH) != 0) {
        ESP_LOGE(TAG, "rename %s -> %s failed", TMP_PATH, RING_PATH);
        std::remove(TMP_PATH);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "%s written (%u bytes)", RING_PATH, static_cast<unsigned>(body.size()));
    return ESP_OK;
}

}  // namespace ring_store
}  // namespace swan
