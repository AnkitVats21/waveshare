#include "services/http/ControlChannel.h"
#include "services/http/SystemInfo.h"
#include "app/audio/recording/AudioRecorder.h"
#include "media_player/MusicPlaybackService.h"
#include "common/thread_config.h"
#include "core_sysdb/led_types.h"
#include <ArduinoJson.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <esp_app_desc.h>
#include <mdns.h>
#include <algorithm>
#include <cstring>
#include <strings.h>

namespace Services {

namespace {

const char* mediaStateToString(MediaPlaybackState s) {
    switch (s) {
        case MediaPlaybackState::RESOLVING:   return "RESOLVING";
        case MediaPlaybackState::BUFFERING:   return "BUFFERING";
        case MediaPlaybackState::PLAYING:     return "PLAYING";
        case MediaPlaybackState::PAUSED:      return "PAUSED";
        case MediaPlaybackState::ERROR_STATE: return "ERROR";
        default:                              return "IDLE";
    }
}

LedMode ledModeFromString(const char* s) {
    if (!s) return LedMode::OFF;
    if (strcasecmp(s, "solid") == 0)   return LedMode::SOLID;
    if (strcasecmp(s, "blink") == 0)   return LedMode::BLINK;
    if (strcasecmp(s, "breath") == 0)  return LedMode::BREATH;
    if (strcasecmp(s, "rainbow") == 0) return LedMode::RAINBOW;
    return LedMode::OFF;
}

int64_t nowMs() { return esp_timer_get_time() / 1000; }

const char* assistantStateToString(AssistantState s) {
    switch (s) {
        case AssistantState::StartingSession:    return "starting";
        case AssistantState::Connecting:         return "connecting";
        case AssistantState::StreamingUserAudio: return "listening";
        case AssistantState::AssistantSpeaking:  return "speaking";
        case AssistantState::WaitingForFollowup: return "followup";
        case AssistantState::Closing:            return "closing";
        case AssistantState::ErrorCooldown:      return "error";
        default:                                 return "idle";
    }
}

const char* wsStateToString(WsState s) {
    switch (s) {
        case WsState::CONNECTING:  return "connecting";
        case WsState::CONNECTED:   return "connected";
        case WsState::GOING_AWAY:  return "going_away";
        case WsState::ERROR_STATE: return "error";
        default:                   return "disconnected";
    }
}

// Upper bound on queue entries included in each state push.
constexpr size_t PUSH_QUEUE_MAX = 30;

} // namespace

ControlChannel& ControlChannel::getInstance() {
    static ControlChannel s_instance;
    return s_instance;
}

ControlChannel::ControlChannel()
    : ReactorTask({
        .name       = "control_ch",
        .stack_size = 6144,
        .priority   = ThreadConfig::Priority::LOW,
        .core_id    = ThreadConfig::CORE_NETWORK,
        .interest   = COMP::AUDIO | COMP::LED | COMP::MEDIA | COMP::BLUETOOTH | COMP::ASSISTANT | COMP::ALARM
    }) {}

bool ControlChannel::begin() {
    startMdns();
    return true;
}

void ControlChannel::startMdns() {
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set(MDNS_HOSTNAME);
    mdns_instance_name_set("Nexus");

    const esp_app_desc_t* app = esp_app_get_description();
    mdns_txt_item_t txt[] = {
        { "api", "/api/ws" },
        { "ver", app ? app->version : "unknown" },
    };
    const uint16_t port = CONFIG_WAVESHARE_HTTP_FILE_SERVER_PORT;
    mdns_service_add(nullptr, "_nexus", "_tcp", port, txt, sizeof(txt) / sizeof(txt[0]));
    mdns_service_add(nullptr, "_http", "_tcp", port, nullptr, 0);
    ESP_LOGI(TAG, "mDNS: advertising %s.local (_nexus._tcp, port %u)", MDNS_HOSTNAME, (unsigned)port);
}

// ─────────────────────────────────────────────────────────────────────────────
// HTTP server hooks (run on the httpd task)
// ─────────────────────────────────────────────────────────────────────────────

esp_err_t ControlChannel::onWsHandshake(httpd_req_t* req) {
    // Handshake complete: this socket becomes the single client (takeover).
    int fd = httpd_req_to_sockfd(req);
    m_server = req->handle;
    int old_fd = m_client_fd.exchange(fd);
    if (old_fd >= 0 && old_fd != fd) {
        ESP_LOGI(TAG, "New client (fd=%d) takes over from fd=%d", fd, old_fd);
        httpd_sess_trigger_close(req->handle, old_fd);
    } else {
        ESP_LOGI(TAG, "Client connected (fd=%d)", fd);
    }
    m_push_now = true;
    if (getHandle()) xTaskNotify(getHandle(), 0, eSetBits);
    return ESP_OK;
}

esp_err_t ControlChannel::handleWsRequest(httpd_req_t* req) {
    // Only data frames arrive here; the handshake goes to onWsHandshake().
    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) return ret;
    if (frame.len == 0) return ESP_OK;
    if (frame.len > MAX_COMMAND_LEN) {
        ESP_LOGW(TAG, "Command frame too large (%u bytes)", (unsigned)frame.len);
        return ESP_FAIL;
    }

    std::string buf(frame.len, '\0');
    frame.payload = reinterpret_cast<uint8_t*>(&buf[0]);
    ret = httpd_ws_recv_frame(req, &frame, frame.len);
    if (ret != ESP_OK) return ret;

    // Only the current client may issue commands; a taken-over socket may still
    // deliver a frame before its close completes.
    if (frame.type == HTTPD_WS_TYPE_TEXT && httpd_req_to_sockfd(req) == m_client_fd.load()) {
        handleCommand(buf.data(), buf.size());
    }
    return ESP_OK;
}

void ControlChannel::onSocketClosed(int sockfd) {
    int expected = sockfd;
    if (m_client_fd.compare_exchange_strong(expected, -1)) {
        ESP_LOGI(TAG, "Client disconnected (fd=%d)", sockfd);
    }
}

void ControlChannel::onServerStopped() {
    m_client_fd = -1;
    m_server = nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Commands: {"cmd": "<name>", ...}
// ─────────────────────────────────────────────────────────────────────────────

void ControlChannel::handleCommand(const char* json, size_t len) {
    JsonDocument doc;
    if (deserializeJson(doc, json, len)) {
        ESP_LOGW(TAG, "Invalid command JSON");
        return;
    }

    auto& sysdb = EmbeddedSysDb::getInstance();
    auto& music = MusicPlaybackService::getInstance();
    const char* cmd = doc["cmd"] | "";

    if (strcmp(cmd, "volume") == 0) {
        int vol = std::clamp(doc["value"] | 80, 0, 100);
        sysdb.mutate([vol](SystemState& s) { s.audio.speaker_volume = vol; });

    } else if (strcmp(cmd, "mic_gain") == 0) {
        float gain = std::clamp(doc["value"] | 60.0f, 0.0f, 60.0f);
        sysdb.mutate([gain](SystemState& s) { s.audio.mic_gain_db = gain; });

    } else if (strcmp(cmd, "mic_mute") == 0) {
        bool muted = doc["value"] | false;
        sysdb.mutate([muted](SystemState& s) { s.audio.mic_enabled = !muted; });

    } else if (strcmp(cmd, "led") == 0) {
        // Any subset of mode/color/speed_ms may be present; apply each that is.
        JsonVariantConst mode  = doc["mode"];
        JsonVariantConst color = doc["color"];
        JsonVariantConst speed = doc["speed_ms"];
        sysdb.mutate([&](SystemState& s) {
            if (mode.is<const char*>()) s.led.mode = ledModeFromString(mode.as<const char*>());
            if (color.is<JsonObjectConst>()) {
                s.led.color = RgbColor{
                    static_cast<uint8_t>(std::clamp(color["r"] | 0, 0, 255)),
                    static_cast<uint8_t>(std::clamp(color["g"] | 0, 0, 255)),
                    static_cast<uint8_t>(std::clamp(color["b"] | 0, 0, 255)),
                };
            }
            if (!speed.isNull()) s.led.speed_ms = speed.as<uint32_t>();
        });

    } else if (strcmp(cmd, "action") == 0) {
        const char* action = doc["action"] | "";
        JsonVariantConst value = doc["value"];

        if (strcmp(action, "play") == 0) {
            const char* stream_url = doc["stream_url"] | "";
            if (stream_url[0] != '\0') {
                InvidiousTrack track;
                track.videoId = doc["id"] | "";
                track.title = doc["title"] | "Unknown Title";
                track.author = doc["artist"] | "Unknown Artist";
                track.durationSeconds = doc["duration"] | 0;
                music.playDirect(track, stream_url);
            } else {
                const char* query = doc["data"] | "";
                if (query[0] != '\0') music.play(query);
            }
        } else if (strcmp(action, "pause") == 0) {
            music.pause();
        } else if (strcmp(action, "resume") == 0) {
            music.resume();
        } else if (strcmp(action, "toggle") == 0) {
            music.postCommand(MediaCmdType::TOGGLE_PLAY_PAUSE);
        } else if (strcmp(action, "stop") == 0) {
            music.stop();
        } else if (strcmp(action, "next") == 0) {
            music.next();
        } else if (strcmp(action, "prev") == 0 || strcmp(action, "previous") == 0) {
            music.previous();
        } else if (strcmp(action, "seek") == 0) {
            music.seekTo(value.as<uint32_t>());
        } else if (strcmp(action, "repeat") == 0) {
            music.setRepeatMode(static_cast<RepeatMode>(std::clamp(value.as<int>(), 0, 2)));
        } else if (strcmp(action, "autoplay") == 0) {
            music.setAutoplay(value.as<bool>());
        } else if (strcmp(action, "caching") == 0) {
            music.setCaching(value.as<bool>());
        } else if (strcmp(action, "queue_add") == 0) {
            // {"id","title","artist","duration","front"}: a track the client already
            // identified (search result or library entry); resolved when it plays.
            InvidiousTrack track;
            track.videoId = doc["id"] | "";
            track.title = doc["title"] | "Unknown Title";
            track.author = doc["artist"] | "Unknown Artist";
            track.durationSeconds = doc["duration"] | 0;
            if (!track.videoId.empty()) music.enqueueTrack(track, doc["front"] | false);
        } else if (strcmp(action, "queue_remove") == 0) {
            music.removeFromQueue(value.as<size_t>());
        } else if (strcmp(action, "queue_clear") == 0) {
            music.clearQueue();
        } else if (strcmp(action, "queue_shuffle") == 0) {
            music.shuffleQueue();
        } else {
            ESP_LOGW(TAG, "Unknown action '%s'", action);
        }

    } else {
        ESP_LOGW(TAG, "Unknown cmd '%s'", cmd);
        return;
    }

    // Queue edits don't touch SysDb, so push the new state explicitly.
    m_push_now = true;
    if (getHandle()) xTaskNotify(getHandle(), 0, eSetBits);
}

// ─────────────────────────────────────────────────────────────────────────────
// State push
// ─────────────────────────────────────────────────────────────────────────────

void ControlChannel::onStateChanged(ComponentMask /*changed*/, const SystemState& /*snap*/) {
    // Pushes are driven from run(); any watched change just marks state dirty.
}

void ControlChannel::pushState() {
    httpd_handle_t server = m_server.load();
    int fd = m_client_fd.load();
    if (!server || fd < 0) return;

    SystemState snap = EmbeddedSysDb::getInstance().snapshot();
    JsonDocument doc;

    doc["board"] = "Nexus (ESP32-S3)";
    doc["connected"] = true;
    doc["up"] = static_cast<uint64_t>(esp_timer_get_time() / 1000000ULL);

    int cpu0 = 0, cpu1 = 0;
    SystemInfo::getCpuUsage(cpu0, cpu1);
    doc["c0"] = cpu0;
    doc["c1"] = cpu1;
    doc["sram"] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    doc["min_sram"] = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    doc["psram"] = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    wifi_ap_record_t ap = {};
    doc["rssi"] = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;

    JsonObject st = doc["state"].to<JsonObject>();
    st["speaker_volume"] = snap.audio.speaker_volume;
    st["mic_gain_db"] = snap.audio.mic_gain_db;
    st["mic_enabled"] = snap.audio.mic_enabled;
    st["is_recording"] = AudioRecorder::getInstance().isRecording();
    st["sample_rate"] = snap.audio.sample_rate;

    JsonObject led = doc["led"].to<JsonObject>();
    led["mode"] = static_cast<int>(snap.led.mode);
    JsonObject color = led["color"].to<JsonObject>();
    color["r"] = snap.led.color.r;
    color["g"] = snap.led.color.g;
    color["b"] = snap.led.color.b;
    led["speed_ms"] = snap.led.speed_ms;

    JsonObject music = doc["music"].to<JsonObject>();
    music["state"] = mediaStateToString(snap.media.state);
    JsonObject track = music["current_track"].to<JsonObject>();
    track["id"] = snap.media.active_song_id;
    track["title"] = snap.media.title;
    track["artist"] = snap.media.artist;
    track["duration"] = snap.media.duration_ms / 1000;
    music["position_ms"] = snap.media.position_ms;
    music["duration_ms"] = snap.media.duration_ms;
    music["seekable"] = snap.media.seekable;
    music["repeat_mode"] = snap.media.repeat_mode;
    music["autoplay"] = snap.media.autoplay_enabled;
    music["caching"] = snap.media.cache_downloads;

    JsonArray queue = music["queue"].to<JsonArray>();
    const auto upcoming = MusicPlaybackService::getInstance().getQueue();
    music["queue_length"] = upcoming.size();
    for (size_t i = 0; i < upcoming.size() && i < PUSH_QUEUE_MAX; ++i) {
        JsonObject item = queue.add<JsonObject>();
        item["id"] = upcoming[i].videoId;
        item["title"] = upcoming[i].title;
        item["artist"] = upcoming[i].author;
        item["duration"] = upcoming[i].durationSeconds;
    }

    JsonObject assistant = doc["assistant"].to<JsonObject>();
    assistant["state"] = assistantStateToString(snap.assistant.session_state);
    assistant["connection"] = wsStateToString(snap.assistant.ws_state);

    JsonObject alarm = doc["alarm"].to<JsonObject>();
    alarm["ringing"] = snap.alarm.playing;
    alarm["id"] = snap.alarm.active_alarm_id;

    JsonObject bt = doc["bluetooth"].to<JsonObject>();
    bt["connected"] = snap.bluetooth.connected;
    bt["device_name"] = snap.bluetooth.device_name;

    std::string out;
    serializeJson(doc, out);

    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = reinterpret_cast<uint8_t*>(out.data());
    frame.len = out.size();
    frame.final = true;
    esp_err_t err = httpd_ws_send_data(server, fd, &frame);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Push to fd=%d failed (%s); dropping client", fd, esp_err_to_name(err));
        int expected = fd;
        if (m_client_fd.compare_exchange_strong(expected, -1)) {
            httpd_sess_trigger_close(server, fd);
        }
    }
}

void ControlChannel::run() {
    bool dirty = false;
    int64_t last_push_ms = 0;

    while (m_running) {
        int64_t since_push = nowMs() - last_push_ms;
        uint32_t wait_ms;
        if (m_client_fd.load() < 0) {
            wait_ms = portMAX_DELAY;
        } else if (dirty) {
            wait_ms = since_push >= MIN_PUSH_INTERVAL_MS ? 0 : MIN_PUSH_INTERVAL_MS - since_push;
        } else {
            wait_ms = since_push >= TELEMETRY_PERIOD_MS ? 0 : TELEMETRY_PERIOD_MS - since_push;
        }

        uint32_t bits = 0;
        TickType_t ticks = (wait_ms == portMAX_DELAY) ? portMAX_DELAY : pdMS_TO_TICKS(wait_ms);
        if (xTaskNotifyWait(0, 0xFFFFFFFF, &bits, ticks) == pdTRUE && bits != 0) {
            dirty = true;
        }
        if (!m_running) break;

        if (m_client_fd.load() < 0) {
            dirty = false;
            continue;
        }

        since_push = nowMs() - last_push_ms;
        bool push_now = m_push_now.exchange(false);
        if (push_now ||
            (dirty && since_push >= MIN_PUSH_INTERVAL_MS) ||
            since_push >= TELEMETRY_PERIOD_MS) {
            pushState();
            last_push_ms = nowMs();
            dirty = false;
        }
    }
}

} // namespace Services
