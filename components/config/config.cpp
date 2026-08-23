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

// spec 11 name                 NVS key (<= 15 chars)
// motion.cal[5]                m_cal        (blob of 5 x int32)
// motion.flaps_s_normal        m_fs_norm
// motion.flaps_s_alarm         m_fs_alrm
// motion.flaps_s_home          m_fs_home
// motion.accel                 m_accel
// motion.hall_tol              m_hall_tol
// motion.en_idle_off           m_en_idle
// motion.hall_active_low       m_hall_lo
// clock.h24                    c_h24
// clock.land_on_tick           c_lot
// clock.granularity_min        c_gran
// countdown.seconds_mode       cd_secmode   (0 minutes, 1 tens, 2 seconds)
// countdown.seconds_live_s     cd_live_s
// time.tz                      t_tz
// time.ntp                     t_ntp
// msg.dwell_s                  msg_dwell
// countdown.land_on_tick       cd_lot
// countdown.zero_hold_s        cd_hold
// countdown.spin_s             cd_spin
// countdown.failure_timeout_s  cd_fail_to
// countdown.reveal[5]          cd_reveal    (blob of 5 x int32)
// (countdown deadline state)   cd_phase / cd_target / cd_seq / cd_setby
// column.mode[5]               col_mode     (blob of 5 x uint8)
// column.maintenance           maint
// wifi.ssid                    w_ssid
// wifi.pass                    w_pass
constexpr const char* K_CAL = "m_cal";
constexpr const char* K_FS_NORM = "m_fs_norm";
constexpr const char* K_FS_ALRM = "m_fs_alrm";
constexpr const char* K_FS_HOME = "m_fs_home";
constexpr const char* K_ACCEL = "m_accel";
constexpr const char* K_HALL_TOL = "m_hall_tol";
constexpr const char* K_EN_IDLE = "m_en_idle";
constexpr const char* K_HALL_LO = "m_hall_lo";
constexpr const char* K_H24 = "c_h24";
constexpr const char* K_CLK_LOT = "c_lot";
constexpr const char* K_GRAN = "c_gran";
constexpr const char* K_SECMODE = "cd_secmode";
constexpr const char* K_CD_LIVE = "cd_live_s";
constexpr const char* K_TZ = "t_tz";
constexpr const char* K_NTP = "t_ntp";
constexpr const char* K_MSG_DWELL = "msg_dwell";
constexpr const char* K_CD_LOT = "cd_lot";
constexpr const char* K_CD_HOLD = "cd_hold";
constexpr const char* K_CD_SPIN = "cd_spin";
constexpr const char* K_CD_FAIL_TO = "cd_fail_to";
constexpr const char* K_CD_REVEAL = "cd_reveal";
constexpr const char* K_CD_PHASE = "cd_phase";
constexpr const char* K_CD_TARGET = "cd_target";
constexpr const char* K_CD_SEQ = "cd_seq";
constexpr const char* K_CD_SETBY = "cd_setby";
constexpr const char* K_COL_MODE = "col_mode";
constexpr const char* K_MAINT = "maint";
constexpr const char* K_WIFI_SSID = "w_ssid";
constexpr const char* K_WIFI_PASS = "w_pass";

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

namespace {

void get_str(nvs_handle_t h, const char* key, std::string* v) {
    size_t len = 0;
    if (nvs_get_str(h, key, nullptr, &len) != ESP_OK || len == 0 || len > 128) return;
    std::string tmp(len, '\0');
    if (nvs_get_str(h, key, tmp.data(), &len) == ESP_OK) {
        tmp.resize(len - 1);  // drop the NUL
        *v = std::move(tmp);
    }
}

}  // namespace

esp_err_t load_app(AppConfig& c) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;  // spec defaults
    if (err != ESP_OK) return err;

    get_bool(h, K_H24, &c.modes.h24);
    get_bool(h, K_CLK_LOT, &c.modes.clock_land_on_tick);
    get_bool(h, K_CD_LOT, &c.modes.cd_land_on_tick);
    // ModesConfig carries plain int; NVS speaks int32_t (long on riscv).
    auto get_int = [&h](const char* key, int* v) {
        int32_t tmp = 0;
        if (nvs_get_i32(h, key, &tmp) == ESP_OK) *v = static_cast<int>(tmp);
    };
    get_int(K_GRAN, &c.modes.granularity_min);
    get_int(K_MSG_DWELL, &c.modes.msg_dwell_s);
    get_int(K_CD_LIVE, &c.modes.seconds_live_s);
    int32_t sec = 0;
    if (nvs_get_i32(h, K_SECMODE, &sec) == ESP_OK) {
        // 0 minutes / 1 tens / 2 seconds.  Anything else is a key written by
        // an older build; fall back to the spec default rather than guess.
        c.modes.seconds_mode = sec == 0   ? SecondsMode::Minutes
                               : sec == 1 ? SecondsMode::Tens
                                          : SecondsMode::Seconds;
    }
    get_int(K_CD_HOLD, &c.modes.zero_hold_s);
    get_int(K_CD_SPIN, &c.modes.spin_s);
    get_int(K_CD_FAIL_TO, &c.modes.failure_timeout_s);
    get_str(h, K_TZ, &c.tz);
    get_str(h, K_NTP, &c.ntp);

    int32_t reveal[N_COLUMNS];
    size_t len = sizeof(reveal);
    if (nvs_get_blob(h, K_CD_REVEAL, reveal, &len) == ESP_OK && len == sizeof(reveal)) {
        for (int i = 0; i < N_COLUMNS; ++i) {
            c.modes.reveal[static_cast<size_t>(i)] = reveal[i];
        }
    }

    nvs_close(h);
    return ESP_OK;
}

esp_err_t load_wifi(WifiConfig& c) {
    nvs_handle_t h;
    const esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;  // never provisioned
    if (err != ESP_OK) return err;
    get_str(h, K_WIFI_SSID, &c.ssid);
    get_str(h, K_WIFI_PASS, &c.pass);
    nvs_close(h);
    return ESP_OK;
}

esp_err_t save_wifi(const WifiConfig& c) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    ESP_ERROR_CHECK(nvs_set_str(h, K_WIFI_SSID, c.ssid.c_str()));
    ESP_ERROR_CHECK(nvs_set_str(h, K_WIFI_PASS, c.pass.c_str()));
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t load_columns(ColumnConfig& c) {
    nvs_handle_t h;
    const esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    // A missing namespace is the fresh-NVS case, and it must leave every
    // column REAL and maintenance off - the ColumnConfig defaults.
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;

    uint8_t modes[N_COLUMNS];
    size_t len = sizeof(modes);
    if (nvs_get_blob(h, K_COL_MODE, modes, &len) == ESP_OK && len == sizeof(modes)) {
        for (int i = 0; i < N_COLUMNS; ++i) {
            // Anything unrecognised reads as Real.  Failing safe means failing
            // towards the real mechanism, never towards a simulation.
            c.mode[static_cast<size_t>(i)] = modes[i] <= static_cast<uint8_t>(ColumnMode::Disabled)
                                                 ? static_cast<ColumnMode>(modes[i])
                                                 : ColumnMode::Real;
        }
    }
    get_bool(h, K_MAINT, &c.maintenance);
    nvs_close(h);
    return ESP_OK;
}

esp_err_t save_columns(const ColumnConfig& c) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    uint8_t modes[N_COLUMNS];
    for (int i = 0; i < N_COLUMNS; ++i) {
        modes[i] = static_cast<uint8_t>(c.mode[static_cast<size_t>(i)]);
    }
    ESP_ERROR_CHECK(nvs_set_blob(h, K_COL_MODE, modes, sizeof(modes)));
    ESP_ERROR_CHECK(nvs_set_u8(h, K_MAINT, c.maintenance ? 1 : 0));
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t save_app(const AppConfig& c) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    ESP_ERROR_CHECK(nvs_set_u8(h, K_H24, c.modes.h24 ? 1 : 0));
    ESP_ERROR_CHECK(nvs_set_u8(h, K_CLK_LOT, c.modes.clock_land_on_tick ? 1 : 0));
    ESP_ERROR_CHECK(nvs_set_u8(h, K_CD_LOT, c.modes.cd_land_on_tick ? 1 : 0));
    ESP_ERROR_CHECK(nvs_set_i32(h, K_GRAN, c.modes.granularity_min));
    ESP_ERROR_CHECK(nvs_set_i32(h, K_MSG_DWELL, c.modes.msg_dwell_s));
    ESP_ERROR_CHECK(nvs_set_i32(
        h, K_SECMODE,
        c.modes.seconds_mode == SecondsMode::Minutes ? 0
        : c.modes.seconds_mode == SecondsMode::Tens  ? 1
                                                     : 2));
    ESP_ERROR_CHECK(nvs_set_i32(h, K_CD_LIVE, c.modes.seconds_live_s));
    ESP_ERROR_CHECK(nvs_set_i32(h, K_CD_HOLD, c.modes.zero_hold_s));
    ESP_ERROR_CHECK(nvs_set_i32(h, K_CD_SPIN, c.modes.spin_s));
    ESP_ERROR_CHECK(nvs_set_i32(h, K_CD_FAIL_TO, c.modes.failure_timeout_s));
    ESP_ERROR_CHECK(nvs_set_str(h, K_TZ, c.tz.c_str()));
    ESP_ERROR_CHECK(nvs_set_str(h, K_NTP, c.ntp.c_str()));

    int32_t reveal[N_COLUMNS];
    for (int i = 0; i < N_COLUMNS; ++i) reveal[i] = c.modes.reveal[static_cast<size_t>(i)];
    ESP_ERROR_CHECK(nvs_set_blob(h, K_CD_REVEAL, reveal, sizeof(reveal)));

    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

namespace {

// The deadline survives a power cycle mid-countdown (spec 7.3) - with or
// without a broker.  One write per set, never per tick.
class NvsCountdownStore final : public CountdownStore {
public:
    bool load(CdPersist& out) override {
        nvs_handle_t h;
        if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
        uint8_t phase = 0;
        int64_t target = 0;
        uint32_t seq = 0;
        const bool ok = nvs_get_u8(h, K_CD_PHASE, &phase) == ESP_OK &&
                        nvs_get_i64(h, K_CD_TARGET, &target) == ESP_OK &&
                        nvs_get_u32(h, K_CD_SEQ, &seq) == ESP_OK;
        // Its OWN key, deliberately, rather than widening a blob: a blob whose
        // size changes is dropped whole on the next boot (see the col_mode
        // length check below), which would silently take the deadline with it
        // across exactly the OTA this field exists to survive.  A missing key
        // costs the origin and nothing else.
        uint8_t setby = 0;
        const bool have_setby = nvs_get_u8(h, K_CD_SETBY, &setby) == ESP_OK;
        nvs_close(h);
        if (!ok || phase > static_cast<uint8_t>(CdPhase::Reveal)) return false;
        out.phase = static_cast<CdPhase>(phase);
        out.set_by = (have_setby && setby <= static_cast<uint8_t>(Origin::Ha))
                         ? static_cast<Origin>(setby)
                         : Origin::Unknown;
        out.target_utc = target;
        out.seq = seq;
        return true;
    }

    void save(const CdPersist& s) override {
        nvs_handle_t h;
        if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
        nvs_set_u8(h, K_CD_PHASE, static_cast<uint8_t>(s.phase));
        nvs_set_i64(h, K_CD_TARGET, s.target_utc);
        nvs_set_u32(h, K_CD_SEQ, s.seq);
        nvs_set_u8(h, K_CD_SETBY, static_cast<uint8_t>(s.set_by));
        nvs_commit(h);
        nvs_close(h);
    }
};

NvsCountdownStore g_cd_store;

}  // namespace

CountdownStore& countdown_store() { return g_cd_store; }

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
