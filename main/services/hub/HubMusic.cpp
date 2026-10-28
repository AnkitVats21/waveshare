#include "services/hub/HubMusic.h"
#include "services/hub/HubLink.h"
#include "media_player/MusicPlaybackService.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "common/thread_config.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include <ArduinoJson.h>
#include <cstring>

namespace Services {

namespace {

const char* stateName(MediaPlaybackState s) {
    switch (s) {
        case MediaPlaybackState::PLAYING:   return "playing";
        case MediaPlaybackState::PAUSED:    return "paused";
        case MediaPlaybackState::RESOLVING:
        case MediaPlaybackState::BUFFERING: return "buffering";
        default:                            return "idle";
    }
}

MediaPlaybackState stateFrom(const char* s) {
    if (strcmp(s, "playing") == 0) return MediaPlaybackState::PLAYING;
    if (strcmp(s, "paused") == 0) return MediaPlaybackState::PAUSED;
    if (strcmp(s, "buffering") == 0) return MediaPlaybackState::BUFFERING;
    if (strcmp(s, "resolving") == 0) return MediaPlaybackState::RESOLVING;
    return MediaPlaybackState::IDLE;
}

bool sessionOn(AssistantState s) {
    return s != AssistantState::Idle && s != AssistantState::ErrorCooldown;
}

InvidiousTrack trackFrom(JsonObjectConst t) {
    InvidiousTrack track;
    track.videoId = t["id"] | "";
    track.title = t["title"] | "";
    track.author = t["artist"] | "";
    track.durationSeconds = t["duration"] | 0;
    return track;
}

void copyStr(char* dst, size_t size, const char* src) {
    strncpy(dst, src ? src : "", size - 1);
    dst[size - 1] = '\0';
}

} // namespace

HubMusic& HubMusic::getInstance() {
    static HubMusic instance;
    return instance;
}

HubMusic::HubMusic()
    : ReactorTask({
          "hub_music",
          STACK_SIZE,
          ThreadConfig::Priority::NORMAL,
          ThreadConfig::CORE_NETWORK,
          COMP::MEDIA | COMP::ASSISTANT
      })
{
    m_rx_queue = xQueueCreate(QUEUE_LEN, sizeof(std::string*));
    m_tx_queue = xQueueCreate(QUEUE_LEN, sizeof(std::string*));
    assert(m_rx_queue && m_tx_queue);
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char id[13];
    snprintf(id, sizeof(id), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    m_id = id;
    HubLink::getInstance().setMusicListener({onMessage, onLinkChange, this});
}

// ─────────────────────────────────────────────────────────────────────────────
// MusicHub (any task)
// ─────────────────────────────────────────────────────────────────────────────

bool HubMusic::active() const {
    return HubLink::getInstance().musicAvailable();
}

bool HubMusic::queueTx(std::string* msg) {
    if (xQueueSend(m_tx_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Send queue full; dropped a message");
        delete msg;
        return false;
    }
    xTaskNotify(m_task_handle, BIT_TX, eSetBits);
    return true;
}

bool HubMusic::sendAction(const Action& a) {
    if (!active() || !a.action) return false;
    JsonDocument doc;
    doc["type"] = "music_action";
    doc["action"] = a.action;
    if (a.query) doc["query"] = a.query;
    if (a.id) doc["id"] = a.id;
    if (a.cached) doc["cached"] = true;
    if (a.position_ms >= 0) doc["position_ms"] = a.position_ms;
    if (a.index >= 0) doc["index"] = a.index;
    if (a.mode) doc["mode"] = a.mode;
    if (a.on >= 0) doc["on"] = a.on == 1;
    if (a.target) doc["target"] = a.target;
    auto* msg = new std::string();
    serializeJson(doc, *msg);
    ESP_LOGI(TAG, "Action to the hub: %s", a.action);
    return queueTx(msg);
}

void HubMusic::playerEvent(uint32_t seq, const char* id, const char* event, const char* reason) {
    JsonDocument doc;
    doc["type"] = "music_event";
    doc["seq"] = seq;
    doc["id"] = id ? id : "";
    doc["event"] = event;
    if (reason) doc["reason"] = reason;
    auto* msg = new std::string();
    serializeJson(doc, *msg);
    ESP_LOGI(TAG, "Track %s [%s] (seq %u)", event, id ? id : "", (unsigned)seq);
    queueTx(msg);
}

// ─────────────────────────────────────────────────────────────────────────────
// From the link (its task)
// ─────────────────────────────────────────────────────────────────────────────

void HubMusic::onMessage(const std::string& text, void* ctx) {
    auto* self = static_cast<HubMusic*>(ctx);
    auto* msg = new std::string(text);
    if (xQueueSend(self->m_rx_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Receive queue full; dropped a hub message");
        delete msg;
        return;
    }
    xTaskNotify(self->m_task_handle, BIT_RX, eSetBits);
}

void HubMusic::onLinkChange(bool, void* ctx) {
    auto* self = static_cast<HubMusic*>(ctx);
    xTaskNotify(self->m_task_handle, BIT_LINK, eSetBits);
}

// ─────────────────────────────────────────────────────────────────────────────
// This task
// ─────────────────────────────────────────────────────────────────────────────

void HubMusic::run() {
    auto& sysdb = EmbeddedSysDb::getInstance();
    while (m_running) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(1000));

        if (bits & BIT_RX) {
            std::string* msg = nullptr;
            while (xQueueReceive(m_rx_queue, &msg, 0) == pdTRUE) {
                handleMessage(*msg);
                delete msg;
            }
        }
        if (bits & BIT_LINK) linkChanged();
        if (bits & BIT_TX) {
            std::string* msg = nullptr;
            while (xQueueReceive(m_tx_queue, &msg, 0) == pdTRUE) {
                if (active() && !HubLink::getInstance().sendText(*msg)) {
                    ESP_LOGW(TAG, "Not sent to the hub");
                }
                delete msg;
            }
        }
        uint32_t db_changed = bits & ~(BIT_RX | BIT_TX | BIT_LINK);
        if (db_changed) {
            m_last_changed = db_changed;
            onStateChanged(db_changed, sysdb.snapshot());
        } else if (active()) {
            report(sysdb.snapshot(), false);   // the 1 s tick
        }
    }
}

void HubMusic::onStateChanged(ComponentMask changed, const SystemState& snap) {
    if (!active()) return;
    if (changed & COMP::ASSISTANT) {
        bool on = sessionOn(snap.assistant.session_state);
        if (on != m_session) {
            m_session = on;
            JsonDocument doc;
            doc["type"] = "voice_activity";
            doc["active"] = on;
            std::string out;
            serializeJson(doc, out);
            HubLink::getInstance().sendText(out);
        }
    }
    if (changed & COMP::MEDIA) report(snap, false);
}

// music_report: on a change of state or song, and every second while playing.
void HubMusic::report(const SystemState& snap, bool force) {
    if (m_mirroring) return;   // the media state is the other edge's
    const char* state = stateName(snap.media.state);
    const char* id = snap.media.active_song_id;
    int64_t now = esp_timer_get_time();
    bool changed = m_last_state != state || m_last_id != id;
    bool due = strcmp(state, "playing") == 0 && now - m_last_report_us >= REPORT_EVERY_US;
    if (!force && !changed && !due) return;
    m_last_state = state;
    m_last_id = id;
    m_last_report_us = now;

    JsonDocument doc;
    doc["type"] = "music_report";
    doc["seq"] = MusicPlaybackService::getInstance().hubSeq();
    doc["id"] = id;
    doc["state"] = state;
    doc["position_ms"] = snap.media.position_ms;
    doc["duration_ms"] = snap.media.duration_ms;
    std::string out;
    serializeJson(doc, out);
    HubLink::getInstance().sendText(out);
}

void HubMusic::linkChanged() {
    auto snap = EmbeddedSysDb::getInstance().snapshot();
    if (active()) {
        // The board's song and queue become the hub's.
        m_session = sessionOn(snap.assistant.session_state);
        sendSync(snap);
        if (m_session) {
            HubLink::getInstance().sendText("{\"type\":\"voice_activity\",\"active\":true}");
        }
        m_last_state.clear();
        report(snap, true);
        return;
    }
    // Down (or the hub doesn't run the music): this board's own queue from
    // here, and nothing shown of another edge's music.
    if (m_mirroring) unmirror();
    m_session = false;
    MusicPlaybackService::getInstance().hubLost();
}

void HubMusic::sendSync(const SystemState& snap) {
    auto& music = MusicPlaybackService::getInstance();
    music.hubSynced();
    JsonDocument doc;
    doc["type"] = "music_sync";
    const char* state = stateName(snap.media.state);
    doc["state"] = state;
    InvidiousTrack cur = music.getCurrentTrack();
    if (strcmp(state, "idle") != 0 && !cur.videoId.empty() &&
        strncmp(cur.videoId.c_str(), MusicPlaybackService::FILE_TRACK_PREFIX,
                strlen(MusicPlaybackService::FILE_TRACK_PREFIX)) != 0) {
        JsonObject t = doc["track"].to<JsonObject>();
        t["id"] = cur.videoId;
        t["title"] = cur.title;
        t["artist"] = cur.author;
        t["duration"] = cur.durationSeconds;
        doc["position_ms"] = snap.media.position_ms;
        JsonArray q = doc["queue"].to<JsonArray>();
        size_t n = 0;
        for (const auto& e : music.getQueue()) {
            if (n++ == QUEUE_SYNC_MAX) break;
            JsonObject o = q.add<JsonObject>();
            o["id"] = e.videoId;
            o["title"] = e.title;
            o["artist"] = e.author;
            o["duration"] = e.durationSeconds;
        }
        ESP_LOGI(TAG, "Sync: '%s' %s at %u ms, %u queued", cur.title.c_str(), state,
                 (unsigned)snap.media.position_ms, (unsigned)n);
    } else {
        doc["state"] = "idle";
        ESP_LOGI(TAG, "Sync: nothing playing");
    }
    std::string out;
    serializeJson(doc, out);
    HubLink::getInstance().sendText(out);
}

void HubMusic::handleMessage(const std::string& text) {
    JsonDocument doc;
    if (deserializeJson(doc, text)) {
        ESP_LOGW(TAG, "Unreadable hub message (%u bytes)", (unsigned)text.size());
        return;
    }
    auto& music = MusicPlaybackService::getInstance();
    const char* type = doc["type"] | "";
    if (strcmp(type, "music_play") == 0) {
        if (m_mirroring) unmirror();
        InvidiousTrack track = trackFrom(doc["track"]);
        music.hubPlay(track, doc["url"] | "", doc["position_ms"] | 0u, doc["paused"] | false, doc["seq"] | 0u);
        m_last_state.clear();
    } else if (strcmp(type, "music_pause") == 0) {
        music.hubPause();
    } else if (strcmp(type, "music_resume") == 0) {
        music.hubResume();
    } else if (strcmp(type, "music_seek") == 0) {
        music.hubSeek(doc["position_ms"] | 0u);
    } else if (strcmp(type, "music_stop") == 0) {
        music.hubStop();
    } else if (strcmp(type, "music_queue") == 0) {
        std::deque<InvidiousTrack> q;
        for (JsonObjectConst t : doc["tracks"].as<JsonArrayConst>()) q.push_back(trackFrom(t));
        const char* r = doc["repeat"] | "off";
        RepeatMode repeat = strcmp(r, "one") == 0 ? RepeatMode::One
                          : strcmp(r, "all") == 0 ? RepeatMode::All : RepeatMode::Off;
        music.hubQueue(std::move(q), repeat, doc["autoplay"] | true);
    } else if (strcmp(type, "music_state") == 0) {
        const char* output = doc["output"] | "";
        Speakers sp;
        sp.here = m_id == output;
        for (JsonObjectConst p : doc["players"].as<JsonArrayConst>()) {
            const char* pid = p["id"] | "";
            const char* name = p["name"] | pid;
            if (m_id == pid) continue;
            sp.names.emplace_back(name);
            if (strcmp(pid, output) == 0) sp.playing_on = name;
        }
        {
            std::lock_guard<std::mutex> lock(m_speakers_mu);
            m_speakers = std::move(sp);
        }
        if (output[0] != '\0' && m_id != output) {
            JsonObjectConst t = doc["track"];
            mirror(output, doc["state"] | "idle", t["id"] | "", t["title"] | "", t["artist"] | "",
                   doc["position_ms"] | 0u, doc["duration_ms"] | 0u);
        } else if (m_mirroring) {
            unmirror();
        }
    }
}

MusicHub::Speakers HubMusic::speakers() const {
    std::lock_guard<std::mutex> lock(m_speakers_mu);
    return m_speakers;
}

// The music plays on another edge: show it in the board's media state as a
// satellite's (dashboard, LCD).
void HubMusic::mirror(const char* output, const char* state, const char* id, const char* title,
                      const char* artist, uint32_t pos_ms, uint32_t dur_ms) {
    if (!m_mirroring) ESP_LOGI(TAG, "The music plays on %s", output);
    m_mirroring = true;
    MediaPlaybackState st = stateFrom(state);
    EmbeddedSysDb::getInstance().mutate([&](SystemState& s) {
        s.media.output_target = MediaOutputTarget::SATELLITE;
        s.media.state = st;
        copyStr(s.media.active_song_id, sizeof(s.media.active_song_id), id);
        copyStr(s.media.title, sizeof(s.media.title), title);
        copyStr(s.media.artist, sizeof(s.media.artist), artist);
        s.media.position_ms = pos_ms;
        s.media.duration_ms = dur_ms;
        s.media.seekable = true;
    });
}

void HubMusic::unmirror() {
    m_mirroring = false;
    EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
        if (s.media.output_target != MediaOutputTarget::SATELLITE) return;
        s.media.output_target = MediaOutputTarget::LOCAL;
        s.media.state = MediaPlaybackState::IDLE;
        s.media.active_song_id[0] = '\0';
        s.media.title[0] = '\0';
        s.media.artist[0] = '\0';
        s.media.position_ms = 0;
        s.media.duration_ms = 0;
        s.media.seekable = false;
    });
}

} // namespace Services
