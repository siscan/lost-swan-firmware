// The IDF implementations of the webapi interfaces (spec 10.2).
//
// webapi is pure so the host dev server and the host tests drive the same
// dispatcher; these are the target-side adapters onto motion::, config:: and
// esp_system.  Nothing here contains policy - every decision is in webapi.
#pragma once

#include "config/config.h"
#include "net/httpd.h"
#include "ring/ring_store.h"
#include "webapi/api.h"

namespace swan {
namespace net {

class IdfMotionAdmin final : public api::MotionAdmin {
public:
    AxisInfo info(int col) override;
    MotionParams params() override;
    void set_params(const MotionParams& p) override;
    bool home(int col) override;
    api::MotionAdmin::CalOutcome adjust_cal(int col, int32_t delta) override;
    bool spin_open_loop(int col, int32_t flaps_s, int seconds) override;
    ColumnConfig columns() override;
    bool set_columns(const ColumnConfig& c) override;
    bool sim_inject(int col, std::string_view kind, int32_t value) override;
    bool sim_available() const override;
};

// Persists to NVS.  The app half also keeps the in-RAM AppConfig current, so
// the tz reported by the state payload and the tz that would survive a reboot
// cannot disagree.
class IdfConfigSink final : public api::ConfigSink {
public:
    explicit IdfConfigSink(config::AppConfig& app) : app_(app) {}
    bool save_motion(const MotionParams& p) override;
    bool save_app(const ModesConfig& m, std::string_view tz,
                  std::string_view ntp) override;

private:
    config::AppConfig& app_;
};

class IdfSysInfo final : public api::SysInfoSource {
public:
    api::SysInfo get() override;
};

class IdfMqttAdmin final : public api::MqttAdmin {
public:
    api::MqttStatus mqtt_status() override;
    bool mqtt_configure(const api::MqttAdmin::MqttSettings& s) override;
};

class IdfAudioAdmin final : public api::AudioAdmin {
public:
    api::AudioState audio_state() override;
    bool audio_set(int volume, bool mute, int quiet_start_min, int quiet_end_min) override;
    bool audio_play(std::string_view cue) override;
    bool audio_stop() override;
};

class IdfWifiAdmin final : public api::WifiAdmin {
public:
    bool set_credentials(std::string_view ssid, std::string_view pass) override;
    bool start_portal() override;
    bool stop_portal() override;
    bool portal_running() override;
    std::string portal_ssid() override;
    bool have_credentials() override;
};

class IdfSystemOps final : public api::SystemOps {
public:
    bool reboot() override;
    bool ota_confirm() override;
    bool ota_rollback() override;
    bool ota_pending_verify() override;
};

}  // namespace net
}  // namespace swan
