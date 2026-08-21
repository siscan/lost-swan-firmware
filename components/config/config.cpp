#include "config/config.h"

#include <cstring>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace swan {
namespace config {
namespace {

constexpr const char* TAG = "config";
constexpr const char* NS = "swan";

// spec 11 name            NVS key (<= 15 chars)
// motion.cal[5]           m_cal        (blob of 5 x int32)
// motion.flaps_s_normal   m_fs_norm
// motion.flaps_s_alarm    m_fs_alrm
// motion.flaps_s_home     m_fs_home
// motion.accel            m_accel
// motion.hall_tol         m_hall_tol
// motion.en_idle_off      m_en_idle
// motion.hall_active_low  m_hall_lo
constexpr const char* K_CAL = "m_cal";
constexpr const char* K_FS_NORM = "m_fs_norm";
constexpr const char* K_FS_ALRM = "m_fs_alrm";
constexpr const char* K_FS_HOME = "m_fs_home";
constexpr const char* K_ACCEL = "m_accel";
constexpr const char* K_HALL_TOL = "m_hall_tol";
constexpr const char* K_EN_IDLE = "m_en_idle";
constexpr const char* K_HALL_LO = "m_hall_lo";

// Leaves *v alone when the key is absent, so defaults survive.
void get_i32(nvs_handle_t h, const char* key, int32_t* v) {
    int32_t tmp = 0;
    if (nvs_get_i32(h, key, &tmp) == ESP_OK) *v = tmp;
}

void get_bool(nvs_handle_t h, const char* key, bool* v) {
    uint8_t tmp = 0;
    if (nvs_get_u8(h, key, &tmp) == ESP_OK) *v = (tmp != 0);
}

}  // namespace

esp_err_t init() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs partition unusable (%s); erasing", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t load(MotionParams& p) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no saved config; running on spec defaults");
        return ESP_OK;
    }
    if (err != ESP_OK) return err;

    get_i32(h, K_FS_NORM, &p.flaps_s_normal);
    get_i32(h, K_FS_ALRM, &p.flaps_s_alarm);
    get_i32(h, K_FS_HOME, &p.flaps_s_home);
    get_i32(h, K_ACCEL, &p.accel);
    get_i32(h, K_HALL_TOL, &p.hall_tol);
    get_bool(h, K_EN_IDLE, &p.en_idle_off);
    get_bool(h, K_HALL_LO, &p.hall_active_low);

    size_t len = sizeof(p.cal);
    int32_t cal[N_COLUMNS];
    if (nvs_get_blob(h, K_CAL, cal, &len) == ESP_OK && len == sizeof(cal)) {
        std::memcpy(p.cal, cal, sizeof(cal));
    }

    nvs_close(h);
    return ESP_OK;
}

esp_err_t save(const MotionParams& p) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    // The step ISR is IRAM-safe and its data is in DRAM, so the flash-cache
    // stall these writes cause does not drop steps (spec 5.2).
    ESP_ERROR_CHECK(nvs_set_i32(h, K_FS_NORM, p.flaps_s_normal));
    ESP_ERROR_CHECK(nvs_set_i32(h, K_FS_ALRM, p.flaps_s_alarm));
    ESP_ERROR_CHECK(nvs_set_i32(h, K_FS_HOME, p.flaps_s_home));
    ESP_ERROR_CHECK(nvs_set_i32(h, K_ACCEL, p.accel));
    ESP_ERROR_CHECK(nvs_set_i32(h, K_HALL_TOL, p.hall_tol));
    ESP_ERROR_CHECK(nvs_set_u8(h, K_EN_IDLE, p.en_idle_off ? 1 : 0));
    ESP_ERROR_CHECK(nvs_set_u8(h, K_HALL_LO, p.hall_active_low ? 1 : 0));
    ESP_ERROR_CHECK(nvs_set_blob(h, K_CAL, p.cal, sizeof(p.cal)));

    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "config saved");
    return err;
}

esp_err_t erase_all() {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    ESP_ERROR_CHECK(nvs_erase_all(h));
    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGW(TAG, "config erased; defaults apply on next boot");
    return err;
}

}  // namespace config
}  // namespace swan
