#include "audio/player.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hal/pins.h"

namespace swan {
namespace audio {
namespace {

constexpr const char* TAG = "audio";

// 1024 samples: ~46 ms at 22050 Hz.  Big enough that a LittleFS read and a
// flash-cache stall from an NVS write cannot empty the DMA queue, small enough
// that stop() takes effect within one buffer rather than a second.
constexpr size_t FRAMES = 1024;

i2s_chan_handle_t g_tx = nullptr;
QueueHandle_t g_cmd = nullptr;
TaskHandle_t g_task = nullptr;

std::mutex g_mu;
AudioSettings g_settings;
std::string g_playing;
bool g_have[CUE_COUNT] = {};
std::atomic<bool> g_stop_now{false};
std::atomic<uint32_t> g_underruns{0};

struct Cmd {
    CueId cue;
    bool stop;
};

AudioSettings snapshot() {
    const std::lock_guard<std::mutex> lock(g_mu);
    return g_settings;
}

// Read a cue's header once, so status() can say which cues actually exist
// rather than discovering it at the moment one is supposed to fire.
void scan_locked() {
    for (size_t i = 0; i < CUE_COUNT; ++i) {
        const std::string p = cue_path(static_cast<CueId>(i));
        std::FILE* f = std::fopen(p.c_str(), "rb");
        if (f == nullptr) {
            g_have[i] = false;
            continue;
        }
        uint8_t head[128];
        const size_t n = std::fread(head, 1, sizeof head, f);
        std::fclose(f);
        g_have[i] = wav_parse(head, n).ok;
    }
}

// Stream one file.  Returns false if it could not be played at all.
bool play_file(CueId c, const CuePolicy& pol, int16_t gain_q8) {
    const std::string path = cue_path(c);
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        ESP_LOGW(TAG, "no file for %s (%s)", cue_id_name(c), path.c_str());
        return false;
    }
    uint8_t head[128];
    const size_t hn = std::fread(head, 1, sizeof head, f);
    const WavInfo w = wav_parse(head, hn);
    if (!w.ok) {
        ESP_LOGW(TAG, "%s: %s", cue_id_name(c), w.err != nullptr ? w.err : "bad wav");
        std::fclose(f);
        return false;
    }

    // The rate is per file, so a 16 kHz cue and a 22.05 kHz one both play at
    // the right pitch instead of whichever was configured first.
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(w.sample_rate);
    i2s_channel_disable(g_tx);
    i2s_channel_reconfig_std_clock(g_tx, &clk);
    i2s_channel_enable(g_tx);

    std::vector<int16_t> buf(FRAMES);
    const int64_t loop_deadline_us =
        pol.loop && pol.max_loop_s > 0
            ? esp_timer_get_time() + static_cast<int64_t>(pol.max_loop_s) * 1000000
            : 0;

    for (;;) {
        std::fseek(f, static_cast<long>(w.data_offset), SEEK_SET);
        uint32_t left = w.data_bytes;
        while (left > 0) {
            if (g_stop_now.load(std::memory_order_relaxed)) {
                std::fclose(f);
                return true;
            }
            const size_t want = std::min<size_t>(buf.size() * 2, left);
            const size_t got = std::fread(buf.data(), 1, want, f);
            if (got == 0) break;
            left -= static_cast<uint32_t>(got);

            // Software gain: there is no hardware volume (spec 9).  32-bit
            // intermediate, because 32767 * 256 overflows an int16 long before
            // the shift brings it back.
            const size_t samples = got / 2;
            for (size_t i = 0; i < samples; ++i) {
                buf[i] = static_cast<int16_t>((static_cast<int32_t>(buf[i]) * gain_q8) >> 8);
            }
            size_t written = 0;
            const esp_err_t err =
                i2s_channel_write(g_tx, buf.data(), samples * 2, &written, pdMS_TO_TICKS(500));
            if (err != ESP_OK || written != samples * 2) {
                g_underruns.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (!pol.loop) break;
        if (loop_deadline_us != 0 && esp_timer_get_time() >= loop_deadline_us) break;
        if (g_stop_now.load(std::memory_order_relaxed)) break;
    }
    std::fclose(f);
    return true;
}

void player_task(void*) {
    for (;;) {
        Cmd c{};
        if (xQueueReceive(g_cmd, &c, portMAX_DELAY) != pdTRUE) continue;
        if (c.stop) continue;   // a stop with nothing playing is a no-op

        const AudioSettings s = snapshot();
        const CuePolicy pol = cue_policy(c.cue, s.failure_loop_s);
        const int16_t gain = volume_scale_q8(s.volume, s.mute);
        {
            const std::lock_guard<std::mutex> lock(g_mu);
            g_playing = cue_id_name(c.cue);
        }
        g_stop_now.store(false, std::memory_order_relaxed);
        play_file(c.cue, pol, gain);
        {
            const std::lock_guard<std::mutex> lock(g_mu);
            g_playing.clear();
        }
        // Silence the amp between cues: a MAX98357A left enabled with no data
        // hisses, and this display spends 99% of its life saying nothing.
        i2s_channel_disable(g_tx);
    }
}

}  // namespace

esp_err_t init(const AudioSettings& s) {
    {
        const std::lock_guard<std::mutex> lock(g_mu);
        g_settings = s;
        scan_locked();
    }

    i2s_chan_config_t ch = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ch.dma_desc_num = 4;
    ch.dma_frame_num = FRAMES / 2;
    ch.auto_clear = true;   // send zeros rather than the last buffer on underrun
    esp_err_t err = i2s_new_channel(&ch, &g_tx, nullptr);
    if (err != ESP_OK) return err;

    i2s_std_config_t cfg = {};
    cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(22050);
    cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_MONO);
    cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;   // the MAX98357A derives its own
    cfg.gpio_cfg.bclk = static_cast<gpio_num_t>(PIN_I2S_BCLK);
    cfg.gpio_cfg.ws = static_cast<gpio_num_t>(PIN_I2S_LRCLK);
    cfg.gpio_cfg.dout = static_cast<gpio_num_t>(PIN_I2S_DIN);
    cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
    err = i2s_channel_init_std_mode(g_tx, &cfg);
    if (err != ESP_OK) return err;

    g_cmd = xQueueCreate(4, sizeof(Cmd));
    if (g_cmd == nullptr) return ESP_ERR_NO_MEM;
    // Priority 4: above httpd, below the modes tick.  A cue that is late is a
    // worse bug than a web page that is, and a cue that delays a frame is
    // worse than both.
    if (xTaskCreate(&player_task, "swan_audio", 4096, nullptr, 4, &g_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    int found = 0;
    for (bool h : g_have) found += h ? 1 : 0;
    ESP_LOGI(TAG, "i2s on bclk %d ws %d dout %d; %d/%d cue files present", PIN_I2S_BCLK,
             PIN_I2S_LRCLK, PIN_I2S_DIN, found, static_cast<int>(CUE_COUNT));
    return ESP_OK;
}

void play(CueId c, int minute_of_day) {
    const AudioSettings s = snapshot();
    if (s.mute) return;
    // Quiet hours [Q8, default off].  An unknown clock plays: silence would be
    // indistinguishable from a broken amp, and a display whose SNTP has not
    // come back should still be able to make a noise.
    if (minute_of_day >= 0 && in_quiet_hours(minute_of_day, s.quiet_start_min, s.quiet_end_min)) {
        ESP_LOGI(TAG, "%s suppressed: quiet hours", cue_id_name(c));
        return;
    }
    const CuePolicy pol = cue_policy(c, s.failure_loop_s);
    bool busy = false;
    {
        const std::lock_guard<std::mutex> lock(g_mu);
        busy = !g_playing.empty();
    }
    // A key click must never cut off the four-minute warning.
    if (busy && !pol.interrupts) return;
    if (busy && pol.interrupts) g_stop_now.store(true, std::memory_order_relaxed);

    Cmd cmd{c, false};
    // Timeout zero: cues fire from the modes task, which owns a 20 Hz tick and
    // must never wait on audio.
    if (g_cmd != nullptr && xQueueSend(g_cmd, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "cue queue full; dropped %s", cue_id_name(c));
    }
}

void stop() { g_stop_now.store(true, std::memory_order_relaxed); }

void set_settings(const AudioSettings& s) {
    const std::lock_guard<std::mutex> lock(g_mu);
    g_settings = s;
}
AudioSettings settings() { return snapshot(); }

Status status() {
    Status s;
    const std::lock_guard<std::mutex> lock(g_mu);
    s.playing = !g_playing.empty();
    s.cue = g_playing;
    std::memcpy(s.have, g_have, sizeof s.have);
    s.underruns = g_underruns.load(std::memory_order_relaxed);
    return s;
}

void rescan() {
    const std::lock_guard<std::mutex> lock(g_mu);
    scan_locked();
}

}  // namespace audio
}  // namespace swan
