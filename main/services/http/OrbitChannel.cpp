#include "services/http/OrbitChannel.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "media_player/MusicPlaybackService.h"
#include <esp_log.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <strings.h>

namespace Services {

namespace {

int64_t nowMs() { return esp_timer_get_time() / 1000; }

void copyField(char* dst, size_t size, const char* src) {
    strncpy(dst, src ? src : "", size - 1);
    dst[size - 1] = '\0';
}

bool containsNoCase(const std::string& hay, const std::string& needle) {
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });
    return it != hay.end();
}

// Words that mean the board's own speaker.
bool isLocalTarget(const std::string& t) {
    static const char* const kWords[] = { "local", "board", "nexus", "device", "speaker", "here" };
    for (const char* w : kWords) {
        if (strcasecmp(t.c_str(), w) == 0) return true;
    }
    return false;
}

// A satellite's playback state as the board's (false: no change).
bool mediaStateFromReport(const char* s, MediaPlaybackState& out) {
    if (strcmp(s, "PLAYING") == 0)   { out = MediaPlaybackState::PLAYING; return true; }
    if (strcmp(s, "PAUSED") == 0)    { out = MediaPlaybackState::PAUSED; return true; }
    if (strcmp(s, "BUFFERING") == 0 || strcmp(s, "RESOLVING") == 0) {
        out = MediaPlaybackState::BUFFERING;
        return true;
    }
    return false;  // IDLE / STOPPED / ERROR: the board decides (track end, error event)
}

struct SendJob {
    httpd_handle_t server;
    int fd;
    std::string text;
};

// Runs on the httpd task.
void sendJob(void* arg) {
    auto* job = static_cast<SendJob*>(arg);
    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = reinterpret_cast<uint8_t*>(job->text.data());
    frame.len = job->text.size();
    frame.final = true;
    // A failed send is caught by the heartbeat (no reports come back).
    httpd_ws_send_frame_async(job->server, job->fd, &frame);
    delete job;
}

} // namespace

OrbitChannel& OrbitChannel::getInstance() {
    static OrbitChannel s_instance;
    return s_instance;
}

void OrbitChannel::begin() {
    MusicPlaybackService::getInstance().setRemoteOutput(this);
    const esp_timer_create_args_t args = {
        .callback = &OrbitChannel::onHeartbeatTimer,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "orbit_hb",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &m_timer) == ESP_OK) {
        esp_timer_start_periodic(m_timer, 1000 * 1000);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// HTTP server hooks (httpd task)
// ─────────────────────────────────────────────────────────────────────────────

esp_err_t OrbitChannel::onWsHandshake(httpd_req_t* req) {
    const int fd = httpd_req_to_sockfd(req);
    m_server = req->handle;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (int i = 0; i < MAX_SATELLITES; ++i) {
        if (m_slots[i].fd < 0) {
            m_slots[i] = Slot{};
            m_slots[i].fd = fd;
            m_slots[i].last_seen_ms = nowMs();
            ESP_LOGI(TAG, "Satellite connected (fd=%d, slot %d)", fd, i);
            return ESP_OK;
        }
    }
    ESP_LOGW(TAG, "Satellite refused (fd=%d): %d already connected", fd, MAX_SATELLITES);
    httpd_sess_trigger_close(req->handle, fd);
    return ESP_OK;
}

esp_err_t OrbitChannel::handleWsRequest(httpd_req_t* req) {
    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) return ret;
    if (frame.len == 0) return ESP_OK;
    if (frame.len > MAX_FRAME_LEN) {
        ESP_LOGW(TAG, "Frame too large (%u bytes)", (unsigned)frame.len);
        return ESP_FAIL;
    }
    std::string buf(frame.len, '\0');
    frame.payload = reinterpret_cast<uint8_t*>(&buf[0]);
    ret = httpd_ws_recv_frame(req, &frame, frame.len);
    if (ret != ESP_OK || frame.type != HTTPD_WS_TYPE_TEXT) return ret;

    int idx;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        idx = findSlot(httpd_req_to_sockfd(req));
        if (idx < 0) return ESP_OK;  // refused at the handshake
        m_slots[idx].last_seen_ms = nowMs();
    }
    handleFrame(idx, buf.data(), buf.size());
    return ESP_OK;
}

void OrbitChannel::onSocketClosed(int sockfd) {
    bool failover = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const int idx = findSlot(sockfd);
        if (idx < 0) return;
        ESP_LOGI(TAG, "Satellite '%s' disconnected (fd=%d)", m_slots[idx].name, sockfd);
        failover = (idx == m_active);
        if (failover) m_active = -1;
        m_slots[idx] = Slot{};
    }
    if (failover) {
        ESP_LOGW(TAG, "Active satellite gone: the music continues on the board");
        MusicPlaybackService::getInstance().switchOutput(false);
    }
}

void OrbitChannel::onServerStopped() {
    bool failover;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        failover = m_active >= 0;
        m_active = -1;
        for (auto& s : m_slots) s = Slot{};
    }
    m_server = nullptr;
    if (failover) MusicPlaybackService::getInstance().switchOutput(false);
}

void OrbitChannel::onHeartbeatTimer(void* arg) {
    auto* self = static_cast<OrbitChannel*>(arg);
    const int64_t now = nowMs();
    std::lock_guard<std::mutex> lock(self->m_mutex);
    for (int i = 0; i < MAX_SATELLITES; ++i) {
        Slot& s = self->m_slots[i];
        if (s.fd >= 0 && !s.closing && now - s.last_seen_ms > TIMEOUT_MS) {
            self->closeLocked(i, "silent");
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Frames from satellites
// ─────────────────────────────────────────────────────────────────────────────

void OrbitChannel::handleFrame(int idx, const char* json, size_t len) {
    JsonDocument doc;
    if (deserializeJson(doc, json, len)) {
        ESP_LOGW(TAG, "Invalid JSON from a satellite");
        return;
    }
    const char* cmd = doc["cmd"] | "";

    if (strcmp(cmd, "satellite_register") == 0) {
        JsonVariantConst sat = doc["satellite"];
        const char* id = sat["id"] | "";
        if (id[0] == '\0') return;
        bool reroute = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            Slot& s = m_slots[idx];
            copyField(s.id, sizeof(s.id), id);
            copyField(s.name, sizeof(s.name), sat["name"] | id);
            copyField(s.type, sizeof(s.type), sat["type"] | "desktop");
            s.registered = true;
            // The same satellite again (reconnected before its old socket
            // timed out): the new socket takes over, and the music with it.
            for (int j = 0; j < MAX_SATELLITES; ++j) {
                if (j == idx || m_slots[j].fd < 0 || strcmp(m_slots[j].id, s.id) != 0) continue;
                if (m_active == j) {
                    m_active = idx;
                    reroute = true;
                }
                closeLocked(j, "replaced");
            }
            ESP_LOGI(TAG, "Satellite '%s' (%s) registered in slot %d", s.name, s.type, idx);
            // Back after the board took the music over (a PC waking from
            // sleep): it may still be playing.
            if (idx != m_active) {
                JsonDocument stopDoc;
                stopDoc["cmd"] = "orbit_stop";
                sendLocked(idx, stopDoc);
            }
        }
        if (reroute) MusicPlaybackService::getInstance().switchOutput(true);

    } else if (strcmp(cmd, "orbit_report") == 0) {
        handleReport(idx, doc);
    } else if (strcmp(cmd, "orbit_event") == 0) {
        handleEvent(idx, doc);
    } else if (strcmp(cmd, "action") == 0) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (idx != m_active) return;  // media keys of an idle satellite
        }
        handleAction(doc);
    } else {
        ESP_LOGD(TAG, "Unknown satellite cmd '%s'", cmd);
    }
}

void OrbitChannel::handleReport(int idx, JsonDocument& doc) {
    const char* state = doc["state"] | "";
    const uint32_t pos = doc["position_ms"] | 0u;
    const uint32_t dur = doc["duration_ms"] | 0u;
    const char* video = doc["video_id"] | "";
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Slot& s = m_slots[idx];
        copyField(s.state, sizeof(s.state), state);
        s.position_ms = pos;
        s.volume = doc["volume"] | 0;
        if (idx != m_active) return;
    }

    // Only reports about the current song: right after a track change the
    // satellite still reports the old one until the new one resolves.
    // An idle or stopped satellite's position is stale.
    MediaPlaybackState ms;
    if (!mediaStateFromReport(state, ms)) return;
    EmbeddedSysDb::getInstance().mutate([&](SystemState& s) {
        if (s.media.output_target != MediaOutputTarget::SATELLITE) return;
        if (strcmp(s.media.active_song_id, video) != 0) return;
        s.media.position_ms = pos;
        if (dur > 0) s.media.duration_ms = dur;
        s.media.state = ms;
    });
}

void OrbitChannel::handleEvent(int idx, JsonDocument& doc) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (idx != m_active) return;
    }
    const char* event = doc["event"] | "";
    const std::string video = doc["video_id"] | "";
    const auto snap = EmbeddedSysDb::getInstance().snapshot();
    if (snap.media.output_target != MediaOutputTarget::SATELLITE || video != snap.media.active_song_id) return;

    auto& music = MusicPlaybackService::getInstance();
    if (strcmp(event, "track_ended") == 0) {
        music.onTrackFinished(video.c_str());
    } else if (strcmp(event, "error") == 0) {
        ESP_LOGW(TAG, "Satellite could not play %s: %s", video.c_str(), doc["message"] | "");
        music.onPlaybackError(video.c_str(), -2);  // not -1: that deletes the board's cached file
    }
}

void OrbitChannel::handleAction(JsonDocument& doc) {
    auto& music = MusicPlaybackService::getInstance();
    const char* action = doc["action"] | "";
    if (strcmp(action, "play") == 0 || strcmp(action, "resume") == 0) music.resume();
    else if (strcmp(action, "pause") == 0) music.pause();
    else if (strcmp(action, "toggle") == 0) music.postCommand(MediaCmdType::TOGGLE_PLAY_PAUSE);
    else if (strcmp(action, "next") == 0) music.next();
    else if (strcmp(action, "prev") == 0 || strcmp(action, "previous") == 0) music.previous();
    else if (strcmp(action, "stop") == 0) music.stop();
}

// ─────────────────────────────────────────────────────────────────────────────
// RemoteOutput: commands to the active satellite
// ─────────────────────────────────────────────────────────────────────────────

void OrbitChannel::play(const InvidiousTrack& track, uint32_t position_ms) {
    JsonDocument doc;
    doc["cmd"] = "orbit_play";
    doc["videoId"] = track.videoId;
    doc["title"] = track.title;
    doc["artist"] = track.author;
    doc["duration"] = track.durationSeconds;
    doc["position_ms"] = position_ms;
    sendToActive(doc);
}

void OrbitChannel::pause() {
    JsonDocument doc;
    doc["cmd"] = "orbit_pause";
    sendToActive(doc);
}

void OrbitChannel::resume() {
    JsonDocument doc;
    doc["cmd"] = "orbit_resume";
    sendToActive(doc);
}

void OrbitChannel::seek(uint32_t position_ms) {
    JsonDocument doc;
    doc["cmd"] = "orbit_seek";
    doc["position_ms"] = position_ms;
    sendToActive(doc);
}

void OrbitChannel::stop() {
    JsonDocument doc;
    doc["cmd"] = "orbit_stop";
    sendToActive(doc);
}

std::vector<RemoteOutput::Satellite> OrbitChannel::satellites() {
    std::vector<Satellite> out;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (int i = 0; i < MAX_SATELLITES; ++i) {
        const Slot& s = m_slots[i];
        if (s.fd >= 0 && s.registered && !s.closing) out.push_back({ s.id, s.name, i == m_active });
    }
    return out;
}

bool OrbitChannel::select(const std::string& target, std::string& chosen) {
    auto& music = MusicPlaybackService::getInstance();
    JsonDocument stopDoc;
    stopDoc["cmd"] = "orbit_stop";

    if (isLocalTarget(target)) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            // Stop it here: once m_active is cleared, the handoff's stop()
            // would go nowhere.
            if (m_active >= 0) sendLocked(m_active, stopDoc);
            m_active = -1;
        }
        chosen = "local";
        music.switchOutput(false);
        return true;
    }

    int found = -1, count = 0, only = -1;
    bool same;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (int i = 0; i < MAX_SATELLITES; ++i) {
            const Slot& s = m_slots[i];
            if (s.fd < 0 || !s.registered || s.closing) continue;
            ++count;
            only = i;
            if (found < 0 && (strcasecmp(s.id, target.c_str()) == 0 ||
                              (!target.empty() && containsNoCase(s.name, target)))) {
                found = i;
            }
        }
        // "my PC" when there is one satellite: that one.
        if (found < 0 && count == 1) found = only;
        if (found < 0) return false;

        same = (found == m_active) &&
               EmbeddedSysDb::getInstance().snapshot().media.output_target == MediaOutputTarget::SATELLITE;
        if (!same) {
            if (m_active >= 0) sendLocked(m_active, stopDoc);
            m_active = found;
        }
        chosen = m_slots[found].name;
    }
    ESP_LOGI(TAG, "Music output: satellite '%s'", chosen.c_str());
    if (!same) music.switchOutput(true);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// State and sending
// ─────────────────────────────────────────────────────────────────────────────

void OrbitChannel::toJson(JsonObject out) {
    std::lock_guard<std::mutex> lock(m_mutex);
    out["active_target"] = m_active >= 0 ? m_slots[m_active].id : "local";
    JsonArray list = out["satellites"].to<JsonArray>();
    for (int i = 0; i < MAX_SATELLITES; ++i) {
        const Slot& s = m_slots[i];
        if (s.fd < 0 || !s.registered || s.closing) continue;
        JsonObject o = list.add<JsonObject>();
        o["id"] = s.id;
        o["name"] = s.name;
        o["type"] = s.type;
        o["state"] = s.state;
        o["position_ms"] = s.position_ms;
        o["volume"] = s.volume;
        o["active"] = (i == m_active);
    }
}

int OrbitChannel::findSlot(int fd) const {
    for (int i = 0; i < MAX_SATELLITES; ++i) {
        if (m_slots[i].fd == fd) return i;
    }
    return -1;
}

void OrbitChannel::sendLocked(int idx, const JsonDocument& doc) {
    httpd_handle_t server = m_server.load();
    const Slot& s = m_slots[idx];
    if (!server || s.fd < 0 || s.closing) return;
    auto* job = new SendJob{ server, s.fd, {} };
    serializeJson(doc, job->text);
    if (httpd_queue_work(server, sendJob, job) != ESP_OK) delete job;
}

void OrbitChannel::sendToActive(const JsonDocument& doc) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_active < 0) {
        ESP_LOGW(TAG, "No active satellite for %s", doc["cmd"].as<const char*>());
        return;
    }
    sendLocked(m_active, doc);
}

void OrbitChannel::closeLocked(int idx, const char* why) {
    Slot& s = m_slots[idx];
    ESP_LOGW(TAG, "Closing satellite '%s' (fd=%d): %s", s.name, s.fd, why);
    s.closing = true;
    httpd_handle_t server = m_server.load();
    // close_fn then calls onSocketClosed, which fails over if it was active.
    if (server) httpd_sess_trigger_close(server, s.fd);
}

} // namespace Services
