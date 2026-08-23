#include "net/mqtt.h"

#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>

#include <sys/time.h>

#include "config/config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "net/wifi.h"
#include "ring/json_write.h"
#include "webapi/mqtt_bridge.h"

namespace swan {
namespace net {
namespace {

constexpr const char* TAG = "mqtt";

// Priority 4: below the 20 Hz modes task (5) so a broker can never delay a
// frame, above httpd (3) so a command from a prop is not stuck behind a page
// load.  esp-mqtt's own default is 5 - identical to the modes task - and it is
// set explicitly below, because forgetting it is silent and the symptom would
// be an occasional late tick nobody could attribute.
constexpr int MQTT_TASK_PRIO = 4;
constexpr int MQTT_TASK_STACK = 8192;   // handle_command builds a JSON DOM

// Inbound: commands from the broker.  Small on purpose - a prop that floods us
// is not owed a backlog, and every dropped command is counted and logged.
constexpr int IN_QUEUE_DEPTH = 8;
constexpr size_t IN_MAX_TOPIC = 96;
constexpr size_t IN_MAX_PAYLOAD = 512;

// Outbound: our own ring, on exactly the shape ws_broadcast uses, and for the
// same reason - a producer must never block on the network.
constexpr size_t OUT_MAX_MSGS = 16;
constexpr size_t OUT_MAX_BYTES = 8192;

struct InMsg {
    char topic[IN_MAX_TOPIC];
    char payload[IN_MAX_PAYLOAD];
    uint16_t topic_len;
    uint16_t payload_len;
};

struct OutMsg {
    std::string topic;
    std::string payload;
    bool retain;
    int qos;
};

api::Context* g_ctx = nullptr;
esp_mqtt_client_handle_t g_client = nullptr;
QueueHandle_t g_in = nullptr;
TaskHandle_t g_task = nullptr;

std::mutex g_mu;                       // guards g_out / g_cfg / g_base
std::deque<OutMsg> g_out;
size_t g_out_bytes = 0;
config::MqttConfig g_cfg;
std::string g_base = "swan/";

std::atomic<bool> g_connected{false};
std::atomic<bool> g_link_up{false};
std::atomic<bool> g_want_reconfigure{false};
std::atomic<bool> g_announce{false};
std::atomic<uint32_t> g_dropped{0};

// Notifications to the transport task, so it never polls.
constexpr uint32_t NOTIFY_WORK = 1;

void kick() {
    if (g_task != nullptr) xTaskNotify(g_task, NOTIFY_WORK, eSetBits);
}

std::string topic_for(const std::string& leaf) {
    const std::lock_guard<std::mutex> lock(g_mu);
    return g_base + leaf;
}

// ---------------------------------------------------------------------------
// The esp-mqtt event handler.
//
// Runs on the CLIENT's task, holding the client's own API lock.  It does three
// things and returns: filter, copy, post.  It must not call handle_command
// (which takes ModeManager's lock, held by the modes task while it publishes
// through our queue) and it must not block.
// ---------------------------------------------------------------------------
void on_mqtt_event(void*, esp_event_base_t, int32_t id, void* data) {
    auto* e = static_cast<esp_mqtt_event_handle_t>(data);
    switch (static_cast<esp_mqtt_event_id_t>(id)) {
        case MQTT_EVENT_CONNECTED: {
            g_connected.store(true, std::memory_order_relaxed);
            ESP_LOGI(TAG, "connected to the broker");
            // Subscribe on EVERY connect: the session is clean by default, so
            // the broker forgets our subscriptions across a reconnect.
            const std::string sub = topic_for(api::TOPIC_CMD_WILDCARD);
            esp_mqtt_client_subscribe(g_client, sub.c_str(), 1);
            g_announce.store(true, std::memory_order_relaxed);
            kick();   // the task re-asserts availability and the retained set
            break;
        }
        case MQTT_EVENT_DISCONNECTED:
            g_connected.store(false, std::memory_order_relaxed);
            ESP_LOGW(TAG, "disconnected from the broker");
            break;

        case MQTT_EVENT_DATA: {
            // Only whole messages.  A fragmented payload delivers the topic on
            // the first chunk only, and reassembling it here would mean holding
            // state across events on the client's task for a command nobody
            // needs to send in pieces.
            if (e->data_len != e->total_data_len || e->current_data_offset != 0) {
                ESP_LOGW(TAG, "ignoring a fragmented message (%d of %d bytes)", e->data_len,
                         e->total_data_len);
                break;
            }
            // A RETAINED command is refused, always.  The broker would replay
            // it on every reconnect and every reboot, so a retained
            // countdown.execute is a countdown that starts itself in an empty
            // room - the same shape as the finished-countdown replay bug.
            if (e->retain) {
                ESP_LOGW(TAG, "refusing a RETAINED command on %.*s - publish it without the "
                              "retain flag, or clear it with an empty retained message",
                         e->topic_len, e->topic);
                break;
            }
            if (e->topic_len <= 0 || static_cast<size_t>(e->topic_len) >= IN_MAX_TOPIC ||
                static_cast<size_t>(e->data_len) >= IN_MAX_PAYLOAD) {
                ESP_LOGW(TAG, "ignoring an oversized message (topic %d, payload %d)",
                         e->topic_len, e->data_len);
                break;
            }
            InMsg m{};
            std::memcpy(m.topic, e->topic, static_cast<size_t>(e->topic_len));
            m.topic_len = static_cast<uint16_t>(e->topic_len);
            if (e->data_len > 0) {
                std::memcpy(m.payload, e->data, static_cast<size_t>(e->data_len));
            }
            m.payload_len = static_cast<uint16_t>(e->data_len);
            // Timeout ZERO, deliberately.  Blocking here would hold the
            // client's API lock while the only task that can drain the queue is
            // waiting for that same lock inside a publish.
            if (xQueueSend(g_in, &m, 0) != pdTRUE) {
                ESP_LOGW(TAG, "command queue full; dropped %.*s", e->topic_len, e->topic);
                g_dropped.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        }
        case MQTT_EVENT_ERROR:
            ESP_LOGW(TAG, "transport error");
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Client lifecycle, all on the transport task
// ---------------------------------------------------------------------------
void client_stop_locked_free() {
    if (g_client == nullptr) return;
    esp_mqtt_client_stop(g_client);
    esp_mqtt_client_destroy(g_client);
    g_client = nullptr;
    g_connected.store(false, std::memory_order_relaxed);
}

void client_start() {
    config::MqttConfig cfg;
    {
        const std::lock_guard<std::mutex> lock(g_mu);
        cfg = g_cfg;
    }
    if (!cfg.configured()) return;
    std::string why;
    if (!api::broker_uri_valid(cfg.uri, why)) {
        ESP_LOGE(TAG, "broker uri rejected (%s): %s", why.c_str(), cfg.uri.c_str());
        return;
    }

    const std::string avail = topic_for(api::TOPIC_AVAILABILITY);

    esp_mqtt_client_config_t c = {};
    c.broker.address.uri = cfg.uri.c_str();
    if (!cfg.user.empty()) {
        c.credentials.username = cfg.user.c_str();
        c.credentials.authentication.password = cfg.pass.c_str();
    }
    // The will covers an UNGRACEFUL drop - power cut, WiFi gone, crash.  A
    // clean stop discards it, which is why mqtt_go_offline exists.
    c.session.last_will.topic = avail.c_str();
    c.session.last_will.msg = api::PAYLOAD_OFFLINE;
    c.session.last_will.msg_len = static_cast<int>(std::strlen(api::PAYLOAD_OFFLINE));
    c.session.last_will.qos = 1;
    c.session.last_will.retain = 1;
    // 30 s rather than the 120 s default: paired with the state topic's 30 s
    // floor, a prop learns the display is gone within ~45 s instead of minutes.
    c.session.keepalive = 30;
    c.network.timeout_ms = 3000;
    c.network.reconnect_timeout_ms = 5000;
    c.task.priority = MQTT_TASK_PRIO;
    c.task.stack_size = MQTT_TASK_STACK;

    g_client = esp_mqtt_client_init(&c);
    if (g_client == nullptr) {
        ESP_LOGE(TAG, "client init failed");
        return;
    }
    esp_mqtt_client_register_event(g_client, MQTT_EVENT_ANY, &on_mqtt_event, nullptr);
    const esp_err_t err = esp_mqtt_client_start(g_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "client start: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(g_client);
        g_client = nullptr;
        return;
    }
    ESP_LOGI(TAG, "connecting to %s (base %s)", cfg.uri.c_str(), g_base.c_str());
}

// One outbound message, published from THIS task only.
//
// esp_mqtt_client_publish, not _enqueue: the client dequeues one outbox item
// per loop iteration and the loop tail parks for a full second, so enqueue
// drains at about one message a second - a connect-time re-assert would take
// half a minute.  publish takes the client's (recursive) API lock, which is
// free while the task is parked, and writes immediately.
bool publish_now(const OutMsg& m) {
    if (g_client == nullptr) return false;
    const std::string topic = topic_for(m.topic);
    const int id = esp_mqtt_client_publish(g_client, topic.c_str(), m.payload.data(),
                                           static_cast<int>(m.payload.size()), m.qos,
                                           m.retain ? 1 : 0);
    return id >= 0;
}

void drain_outbound() {
    for (;;) {
        OutMsg m;
        {
            const std::lock_guard<std::mutex> lock(g_mu);
            if (g_out.empty()) return;
            m = std::move(g_out.front());
            g_out.pop_front();
            g_out_bytes -= m.payload.size();
        }
        if (!publish_now(m)) {
            // Not connected, or the client refused it.  Dropping is correct
            // here: state and countdown are RETAINED and re-asserted on the
            // next connect, so holding a stale document to send later would
            // publish the past.
            g_dropped.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void handle_inbound(const InMsg& m) {
    const std::string_view topic(m.topic, m.topic_len);
    const std::string_view payload(m.payload, m.payload_len);
    std::string base;
    {
        const std::lock_guard<std::mutex> lock(g_mu);
        base = g_base;
    }

    std::string body, cmd;
    const api::CmdParse r = api::command_from_topic(base, topic, payload, body, cmd);
    if (r != api::CmdParse::Ok) {
        // NotOurs is the common case - our own retained publishes come back on
        // the wildcard - and is not worth a line.
        if (r != api::CmdParse::NotOurs) {
            ESP_LOGW(TAG, "rejected topic %.*s", static_cast<int>(topic.size()), topic.data());
        }
        return;
    }

    timeval tv;
    gettimeofday(&tv, nullptr);
    const int64_t utc_ms = static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;

    // THE rule: the same dispatcher every other transport uses.
    const std::string res = api::handle_command(*g_ctx, body, utc_ms, Origin::Mqtt);

    json::Writer w;
    w.obj().kv("cmd", cmd).kv_raw("res", res).end_obj();
    mqtt_publish(api::TOPIC_EVENT, w.take(), false, 0);
}

void transport_task(void*) {
    for (;;) {
        // A reconfiguration is performed HERE, never on the caller's task.
        if (g_want_reconfigure.exchange(false, std::memory_order_relaxed)) {
            client_stop_locked_free();
            {
                const std::lock_guard<std::mutex> lock(g_mu);
                config::load_mqtt(g_cfg);
                g_base = api::normalize_base(g_cfg.base);
                g_out.clear();
                g_out_bytes = 0;
            }
            if (g_link_up.load(std::memory_order_relaxed)) client_start();
        }

        // Freshly connected: say we are here.  Retained, so a prop or Home
        // Assistant that subscribes later learns it without waiting for the
        // next change - which is the whole reason availability is retained
        // rather than announced once.
        if (g_announce.exchange(false, std::memory_order_relaxed)) {
            OutMsg hello{api::TOPIC_AVAILABILITY, api::PAYLOAD_ONLINE, true, 1};
            publish_now(hello);
        }

        InMsg m;
        while (xQueueReceive(g_in, &m, 0) == pdTRUE) handle_inbound(m);
        drain_outbound();

        // Woken by a publish, a command or a link change; the timeout is only
        // a backstop so a missed notification cannot wedge the queue.
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(500));
    }
}

void on_link(bool up) {
    g_link_up.store(up, std::memory_order_relaxed);
    // Only ask; the task does the work.  This runs on the system event task.
    g_want_reconfigure.store(true, std::memory_order_relaxed);
    kick();
}

}  // namespace

esp_err_t mqtt_init(api::Context& ctx) {
    g_ctx = &ctx;
    {
        const std::lock_guard<std::mutex> lock(g_mu);
        config::load_mqtt(g_cfg);
        g_base = api::normalize_base(g_cfg.base);
    }
    if (!g_cfg.enabled) {
        ESP_LOGI(TAG, "disabled (spec 10.0: the display is a clock without a broker)");
    }

    g_in = xQueueCreate(IN_QUEUE_DEPTH, sizeof(InMsg));
    if (g_in == nullptr) return ESP_ERR_NO_MEM;
    if (xTaskCreate(&transport_task, "swan_mqtt", 4096, nullptr, MQTT_TASK_PRIO, &g_task) !=
        pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    on_link_change(&on_link);
    return ESP_OK;
}

esp_err_t mqtt_reconfigure() {
    g_want_reconfigure.store(true, std::memory_order_relaxed);
    kick();
    return ESP_OK;
}

void mqtt_publish(const std::string& topic_leaf, const std::string& payload, bool retain,
                  int qos) {
    if (!g_connected.load(std::memory_order_relaxed)) return;
    {
        const std::lock_guard<std::mutex> lock(g_mu);
        while (!g_out.empty() &&
               (g_out.size() >= OUT_MAX_MSGS || g_out_bytes + payload.size() > OUT_MAX_BYTES)) {
            g_out_bytes -= g_out.front().payload.size();
            g_out.pop_front();
            g_dropped.fetch_add(1, std::memory_order_relaxed);
        }
        g_out_bytes += payload.size();
        g_out.push_back(OutMsg{topic_leaf, payload, retain, qos});
    }
    kick();
}

void mqtt_go_offline() {
    if (g_client == nullptr || !g_connected.load(std::memory_order_relaxed)) return;
    const std::string topic = topic_for(api::TOPIC_AVAILABILITY);
    esp_mqtt_client_publish(g_client, topic.c_str(), api::PAYLOAD_OFFLINE,
                            static_cast<int>(std::strlen(api::PAYLOAD_OFFLINE)), 1, 1);
    // Bounded wait: QoS 1 has no synchronous ack, and a broker that has gone
    // away must not delay a reboot.
    for (int i = 0; i < 20 && g_connected.load(std::memory_order_relaxed); ++i) {
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    esp_mqtt_client_stop(g_client);
    g_connected.store(false, std::memory_order_relaxed);
}

bool mqtt_connected() { return g_connected.load(std::memory_order_relaxed); }
bool mqtt_enabled() {
    const std::lock_guard<std::mutex> lock(g_mu);
    return g_cfg.enabled;
}
uint32_t mqtt_dropped() { return g_dropped.load(std::memory_order_relaxed); }

}  // namespace net
}  // namespace swan
