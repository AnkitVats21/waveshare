#include "StarServer.h"
#include <iostream>
#include <chrono>
#include <algorithm>
#include <nlohmann/json.hpp>
#include "core_sysdb/SystemState.h"

namespace StarReplica {

namespace {

// ── Canonical playback-state vocabulary ────────────────────────────────────
// Every daemon-facing surface (REST + WS snapshot) speaks this vocabulary.
// The ESP's own native HTTP server (main/services/network/HttpFileServerService.cpp)
// uses a different vocabulary (IDLE/STREAMING/LOCAL) - that's a separate,
// unrelated API surface and out of scope here.
const char* mediaStateToString(MediaPlaybackState s) {
    switch (s) {
        case MediaPlaybackState::IDLE:        return "IDLE";
        case MediaPlaybackState::RESOLVING:   return "RESOLVING";
        case MediaPlaybackState::BUFFERING:   return "BUFFERING";
        case MediaPlaybackState::PLAYING:     return "PLAYING";
        case MediaPlaybackState::PAUSED:      return "PAUSED";
        case MediaPlaybackState::ERROR_STATE: return "ERROR";
        default:                              return "IDLE";
    }
}

LedMode ledModeFromString(const std::string& s) {
    if (s == "solid")   return LedMode::SOLID;
    if (s == "blink")   return LedMode::BLINK;
    if (s == "breath")  return LedMode::BREATH;
    if (s == "rainbow") return LedMode::RAINBOW;
    return LedMode::OFF;
}

// Single-byte enum fields (MediaPlaybackState, LedMode, ...) are wire-serialized
// as exactly 1 byte, so ReplicaTable::getUint32/getInt32 (which require >=4
// stored bytes) can't read them. Read the raw byte directly instead.
uint8_t getU8Field(const ReplicaTable& replica, uint8_t comp_id, uint8_t field_tag, uint8_t default_val) {
    std::vector<uint8_t> raw;
    if (replica.getRawField(comp_id, field_tag, raw) && !raw.empty()) {
        return raw[0];
    }
    return default_val;
}

} // namespace

StarServer::StarServer(ReplicaTable& replica)
    : m_replica(replica) {}

StarServer::~StarServer() {
    stop();
}

bool StarServer::start(uint16_t port) {
    if (m_running) return true;

    try {
        m_server.clear_access_channels(websocketpp::log::alevel::all);
        m_server.set_access_channels(websocketpp::log::alevel::connect | websocketpp::log::alevel::disconnect);
        m_server.clear_error_channels(websocketpp::log::elevel::all);

        m_server.init_asio();

        m_server.set_open_handler([this](websocketpp::connection_hdl hdl) {
            onOpen(hdl);
        });
        m_server.set_close_handler([this](websocketpp::connection_hdl hdl) {
            onClose(hdl);
        });
        m_server.set_message_handler([this](websocketpp::connection_hdl hdl, WsServer::message_ptr msg) {
            onMessage(hdl, msg);
        });
        m_server.set_http_handler([this](websocketpp::connection_hdl hdl) {
            onHttp(hdl);
        });

        m_server.set_reuse_addr(true);
        m_server.listen(port);
        m_server.start_accept();

        m_running = true;
        m_server_thread = std::thread([this]() {
            std::cout << "[StarServer] Listening on port for Waveshare client connections & Web Dashboards..." << std::endl;
            m_server.run();
        });

        return true;
    } catch (const std::exception& e) {
        std::cerr << "[StarServer] Error starting server: " << e.what() << std::endl;
        return false;
    }
}

void StarServer::stop() {
    if (!m_running) return;
    m_running = false;

    try {
        m_server.stop_listening();

        websocketpp::connection_hdl waveshare_snapshot;
        bool had_waveshare = false;
        std::vector<websocketpp::connection_hdl> dashboard_snapshot;
        {
            std::lock_guard<std::mutex> lock(m_conn_mutex);
            had_waveshare = m_has_waveshare_conn;
            waveshare_snapshot = m_waveshare_hdl;
            m_has_waveshare_conn = false;
            dashboard_snapshot.assign(m_dashboard_clients.begin(), m_dashboard_clients.end());
            m_dashboard_clients.clear();
        }

        websocketpp::lib::error_code ec;
        if (had_waveshare) {
            m_server.close(waveshare_snapshot, websocketpp::close::status::going_away, "Server shutting down", ec);
        }
        for (auto& hdl : dashboard_snapshot) {
            m_server.close(hdl, websocketpp::close::status::going_away, "Server shutting down", ec);
        }

        m_server.stop();
        if (m_server_thread.joinable()) {
            m_server_thread.join();
        }
    } catch (...) {}
}

bool StarServer::isWaveshareConnected() const {
    std::lock_guard<std::mutex> lock(m_conn_mutex);
    return m_has_waveshare_conn;
}

void StarServer::onOpen(websocketpp::connection_hdl hdl) {
    auto con = m_server.get_con_from_hdl(hdl);
    std::string path = con->get_resource();

    // NOTE: m_server.send() must never be called while holding m_conn_mutex.
    // websocketpp can synchronously invoke another handler (e.g. onClose, for
    // an already-broken peer) from within send(), and since m_conn_mutex is a
    // plain non-recursive std::mutex, that reentrant lock attempt deadlocks
    // the single ASIO thread forever. Snapshot whatever state is needed under
    // the lock, release it, then do all sends.
    if (path == "/api/star/ws") {
        std::vector<websocketpp::connection_hdl> dashboard_snapshot;
        {
            std::lock_guard<std::mutex> lock(m_conn_mutex);
            m_waveshare_hdl = hdl;
            m_has_waveshare_conn = true;
            dashboard_snapshot.assign(m_dashboard_clients.begin(), m_dashboard_clients.end());
        }
        std::cout << "[StarServer] Waveshare device connected. Requesting catch-up from seq "
                  << m_replica.getHeadSeq() << "..." << std::endl;

        auto req = StarProtocol::buildCatchupReqFrame(m_replica.getHeadSeq());
        websocketpp::lib::error_code ec;
        m_server.send(hdl, req.data(), req.size(), websocketpp::frame::opcode::binary, ec);
        if (ec) {
            std::cerr << "[StarServer] Failed to send REQ_CATCHUP: " << ec.message() << std::endl;
        }

        // Notify web dashboard clients
        std::string notif = R"({"type":"device_status","connected":true})";
        for (auto& w_hdl : dashboard_snapshot) {
            m_server.send(w_hdl, notif, websocketpp::frame::opcode::text, ec);
        }
    } else {
        {
            std::lock_guard<std::mutex> lock(m_conn_mutex);
            m_dashboard_clients.insert(hdl);
        }
        std::cout << "[StarServer] Web dashboard client connected (resource=" << path << ")" << std::endl;
        websocketpp::lib::error_code ec;
        m_server.send(hdl, buildSnapshotJson(), websocketpp::frame::opcode::text, ec);
    }
}

void StarServer::onClose(websocketpp::connection_hdl hdl) {
    bool was_waveshare = false;
    std::vector<websocketpp::connection_hdl> dashboard_snapshot;
    {
        std::lock_guard<std::mutex> lock(m_conn_mutex);
        if (m_has_waveshare_conn && !m_waveshare_hdl.owner_before(hdl) && !hdl.owner_before(m_waveshare_hdl)) {
            m_has_waveshare_conn = false;
            was_waveshare = true;
            dashboard_snapshot.assign(m_dashboard_clients.begin(), m_dashboard_clients.end());
        } else {
            m_dashboard_clients.erase(hdl);
        }
    }

    if (was_waveshare) {
        std::cout << "[StarServer] Waveshare device disconnected." << std::endl;
        std::string notif = R"({"type":"device_status","connected":false})";
        websocketpp::lib::error_code ec;
        for (auto& w_hdl : dashboard_snapshot) {
            m_server.send(w_hdl, notif, websocketpp::frame::opcode::text, ec);
        }
    } else {
        std::cout << "[StarServer] Web dashboard client disconnected." << std::endl;
    }
}

void StarServer::onMessage(websocketpp::connection_hdl hdl, WsServer::message_ptr msg) {
    if (msg->get_opcode() == websocketpp::frame::opcode::text) {
        handleDashboardTextMessage(hdl, msg->get_payload());
        return;
    }

    if (msg->get_opcode() != websocketpp::frame::opcode::binary) {
        return;
    }

    const std::string& raw = msg->get_payload();
    if (raw.size() < StarProtocol::HEADER_SIZE) return;

    const auto* data = reinterpret_cast<const uint8_t*>(raw.data());
    size_t len = raw.size();

    uint8_t msg_type = data[0];
    uint16_t payload_len = StarProtocol::readU16BE(&data[1]);
    if (len < StarProtocol::HEADER_SIZE + payload_len) return;

    const uint8_t* p = data + StarProtocol::HEADER_SIZE;

    switch (static_cast<StarProtocol::MsgType>(msg_type)) {
        case StarProtocol::MsgType::WAL_BATCH: {
            size_t offset = 0;
            size_t count = 0;
            while (offset + sizeof(WalWireRecord) <= payload_len) {
                const auto* rec = reinterpret_cast<const WalWireRecord*>(p + offset);
                if (offset + sizeof(WalWireRecord) + rec->val_len > payload_len) break;
                m_replica.applyRecord(rec->seq, rec->component_id, rec->field_tag, rec->value, rec->val_len);
                offset += sizeof(WalWireRecord) + rec->val_len;
                count++;
            }
            if (count > 0) {
                std::cout << "[StarServer] Applied WAL_BATCH (" << count << " records, head_seq="
                          << m_replica.getHeadSeq() << ")" << std::endl;
                broadcastSnapshot();
            }
            break;
        }

        case StarProtocol::MsgType::SNAPSHOT_START: {
            uint32_t head_seq = StarProtocol::readU32LE(p);
            uint16_t total = StarProtocol::readU16BE(&p[4]);
            std::cout << "[StarServer] Starting snapshot sync (head_seq=" << head_seq
                      << ", total_records=" << total << ")" << std::endl;
            m_replica.setHeadSeq(head_seq);
            break;
        }

        case StarProtocol::MsgType::SNAPSHOT_FIELD: {
            if (payload_len < 3) break;
            uint8_t comp_id = p[0];
            uint8_t field_tag = p[1];
            uint8_t val_len = p[2];
            if (payload_len < 3 + val_len) break;
            m_replica.applyRecord(m_replica.getHeadSeq(), comp_id, field_tag, p + 3, val_len);
            break;
        }

        case StarProtocol::MsgType::SNAPSHOT_END: {
            uint32_t head_seq = StarProtocol::readU32LE(p);
            m_replica.setHeadSeq(head_seq);
            std::cout << "[StarServer] Snapshot sync complete! Total fields replicated: "
                      << m_replica.getFieldCount() << " (head_seq=" << head_seq << ")" << std::endl;
            broadcastSnapshot();
            break;
        }

        case StarProtocol::MsgType::CMD_ACK: {
            if (payload_len >= 5) {
                uint8_t status = p[0];
                uint32_t seq = StarProtocol::readU32LE(&p[1]);
                std::cout << "[StarServer] Received CMD_ACK: status=" << (int)status
                          << ", seq=" << seq << std::endl;
            }
            break;
        }

        default:
            std::cout << "[StarServer] Unhandled msg_type: 0x" << std::hex << (int)msg_type << std::dec << std::endl;
            break;
    }
}

// Dashboard -> daemon command envelope: {"cmd": "<name>", ...}
// This is the single, client-agnostic control channel for anything with
// ongoing/live device state (volume, mic, LED, playback). It replaces the
// old per-field REST endpoints (POST /api/audio/volume etc.) - see brief
// bug #4. Kept deliberately client-agnostic (no web-only assumptions) since
// a future native Android client will speak the same envelope.
void StarServer::handleDashboardTextMessage(websocketpp::connection_hdl /*hdl*/, const std::string& text) {
    try {
        auto j = nlohmann::json::parse(text);
        std::string cmd = j.value("cmd", "");

        if (cmd == "volume") {
            int vol = std::clamp(j.value("value", 80), 0, 100);
            sendSetField(static_cast<uint8_t>(ComponentId::AUDIO), TAG_AUDIO::speaker_volume,
                         reinterpret_cast<const uint8_t*>(&vol), sizeof(vol));

        } else if (cmd == "mic_gain") {
            float gain = j.value("value", 60.0f);
            sendSetField(static_cast<uint8_t>(ComponentId::AUDIO), TAG_AUDIO::mic_gain_db,
                         reinterpret_cast<const uint8_t*>(&gain), sizeof(gain));

        } else if (cmd == "mic_mute") {
            bool muted = j.value("value", false);
            uint8_t enabled = muted ? 0 : 1;
            sendSetField(static_cast<uint8_t>(ComponentId::AUDIO), TAG_AUDIO::mic_enabled, &enabled, 1);

        } else if (cmd == "led") {
            // Any subset of mode/color/speed_ms may be present; apply each that is.
            if (j.contains("mode") && j["mode"].is_string()) {
                auto mode = static_cast<uint8_t>(ledModeFromString(j["mode"].get<std::string>()));
                sendSetField(static_cast<uint8_t>(ComponentId::LED), TAG_LED::mode, &mode, 1);
            }
            if (j.contains("color") && j["color"].is_object()) {
                RgbColor c{};
                c.r = static_cast<uint8_t>(std::clamp(j["color"].value("r", 0), 0, 255));
                c.g = static_cast<uint8_t>(std::clamp(j["color"].value("g", 0), 0, 255));
                c.b = static_cast<uint8_t>(std::clamp(j["color"].value("b", 0), 0, 255));
                sendSetField(static_cast<uint8_t>(ComponentId::LED), TAG_LED::color,
                             reinterpret_cast<const uint8_t*>(&c), sizeof(c));
            }
            if (j.contains("speed_ms")) {
                uint32_t speed = j.value("speed_ms", 500u);
                sendSetField(static_cast<uint8_t>(ComponentId::LED), TAG_LED::speed_ms,
                             reinterpret_cast<const uint8_t*>(&speed), sizeof(speed));
            }

        } else if (cmd == "action") {
            std::string action = j.value("action", "");
            if (action == "play") {
                std::string url = j.value("data", "");
                sendExecAction(static_cast<uint8_t>(MediaCmdId::PLAY), 0, 0, url);
            } else if (action == "pause") {
                sendExecAction(static_cast<uint8_t>(MediaCmdId::PAUSE), 0, 0, "");
            } else if (action == "resume") {
                sendExecAction(static_cast<uint8_t>(MediaCmdId::RESUME), 0, 0, "");
            } else if (action == "stop") {
                sendExecAction(static_cast<uint8_t>(MediaCmdId::STOP), 0, 0, "");
            } else if (action == "next") {
                sendExecAction(static_cast<uint8_t>(MediaCmdId::NEXT), 0, 0, "");
            } else if (action == "prev") {
                sendExecAction(static_cast<uint8_t>(MediaCmdId::PREVIOUS), 0, 0, "");
            } else if (action == "seek") {
                uint32_t val = j.value("value", 0);
                sendExecAction(static_cast<uint8_t>(MediaCmdId::SEEK), 0, val, "");
            } else if (action == "autoplay") {
                // MusicPlaybackService::executeAction() already handles this MediaCmdId directly.
                uint32_t on = j.value("value", true) ? 1 : 0;
                sendExecAction(static_cast<uint8_t>(MediaCmdId::AUTOPLAY), 0, on, "");
            } else if (action == "caching") {
                uint32_t on = j.value("value", false) ? 1 : 0;
                sendExecAction(static_cast<uint8_t>(MediaCmdId::CACHING), 0, on, "");
            } else if (action == "repeat") {
                // No dedicated MediaCmdId exists for repeat; TAG_MEDIA::repeat_mode is a
                // Writable replicated field, so it goes through CMD_SET_FIELD instead.
                uint8_t mode = static_cast<uint8_t>(j.value("value", 0));
                sendSetField(static_cast<uint8_t>(ComponentId::MEDIA), TAG_MEDIA::repeat_mode, &mode, 1);
            }
        }
    } catch (...) {}
}

void StarServer::onHttp(websocketpp::connection_hdl hdl) {
    auto con = m_server.get_con_from_hdl(hdl);
    std::string method = con->get_request().get_method();
    std::string resource = con->get_resource();

    std::string path = resource;
    auto qpos = path.find('?');
    if (qpos != std::string::npos) {
        path = path.substr(0, qpos);
    }

    if (method == "OPTIONS") {
        con->set_status(websocketpp::http::status_code::no_content);
        con->append_header("Access-Control-Allow-Origin", "*");
        con->append_header("Access-Control-Allow-Methods", "GET, OPTIONS");
        con->append_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
        return;
    }

    con->append_header("Access-Control-Allow-Origin", "*");
    con->append_header("Access-Control-Allow-Methods", "GET, OPTIONS");
    con->append_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
    con->append_header("Content-Type", "application/json");

    // Read-only REST surface. All live/writable device control now goes over
    // the dashboard WebSocket command channel (see handleDashboardTextMessage).
    if (method == "GET" && path == "/api/status") {
        con->set_status(websocketpp::http::status_code::ok);
        con->set_body(buildSnapshotJson());
        return;
    }

    if (method == "GET" && path == "/api/music/status") {
        auto state = static_cast<MediaPlaybackState>(
            getU8Field(m_replica, static_cast<uint8_t>(ComponentId::MEDIA), TAG_MEDIA::state, 0));
        std::string song_title = m_replica.getString(static_cast<uint8_t>(ComponentId::MEDIA), TAG_MEDIA::title);

        nlohmann::json j;
        j["state"] = mediaStateToString(state);
        nlohmann::json track;
        track["id"] = m_replica.getString(static_cast<uint8_t>(ComponentId::MEDIA), TAG_MEDIA::active_song_id, "");
        track["title"] = song_title;
        track["artist"] = m_replica.getString(static_cast<uint8_t>(ComponentId::MEDIA), TAG_MEDIA::artist, "");
        track["duration"] = m_replica.getUint32(static_cast<uint8_t>(ComponentId::MEDIA), TAG_MEDIA::duration_ms, 0) / 1000;
        j["current_track"] = track;
        j["position_ms"] = m_replica.getUint32(static_cast<uint8_t>(ComponentId::MEDIA), TAG_MEDIA::position_ms, 0);
        j["duration_ms"] = m_replica.getUint32(static_cast<uint8_t>(ComponentId::MEDIA), TAG_MEDIA::duration_ms, 0);
        j["seekable"] = m_replica.getBool(static_cast<uint8_t>(ComponentId::MEDIA), TAG_MEDIA::seekable, false);
        j["repeat_mode"] = m_replica.getUint32(static_cast<uint8_t>(ComponentId::MEDIA), TAG_MEDIA::repeat_mode, 0);
        j["autoplay"] = m_replica.getBool(static_cast<uint8_t>(ComponentId::MEDIA), TAG_MEDIA::autoplay_enabled, true);
        j["caching"] = m_replica.getBool(static_cast<uint8_t>(ComponentId::MEDIA), TAG_MEDIA::cache_downloads, false);
        con->set_status(websocketpp::http::status_code::ok);
        con->set_body(j.dump());
        return;
    }

    con->set_status(websocketpp::http::status_code::not_found);
    con->set_body(R"({"error":"Not found on STAR replica daemon"})");
}

std::string StarServer::buildSnapshotJson() const {
    nlohmann::json j;
    static auto start_time = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    auto uptime_sec = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();

    j["board"] = "Waveshare ESP32-S3 (via STAR Replica)";
    j["connected"] = isWaveshareConnected();
    j["source"] = "star_replica_daemon";
    j["up"] = uptime_sec;
    j["latest_seq"] = m_replica.getHeadSeq();
    // NOTE: no c0/c1/sram/min_sram/psram/rssi fields here - the daemon has no
    // real telemetry channel for these (StarProtocol.h is frozen for this
    // task), so they are omitted rather than fabricated. See brief bug #3.

    // Hardware State
    nlohmann::json st;
    st["speaker_volume"] = m_replica.getInt32(static_cast<uint8_t>(ComponentId::AUDIO), TAG_AUDIO::speaker_volume, 80);
    st["mic_gain_db"] = m_replica.getFloat(static_cast<uint8_t>(ComponentId::AUDIO), TAG_AUDIO::mic_gain_db, 60.0f);
    st["mic_enabled"] = m_replica.getBool(static_cast<uint8_t>(ComponentId::AUDIO), TAG_AUDIO::mic_enabled, true);
    st["is_recording"] = m_replica.getBool(static_cast<uint8_t>(ComponentId::AUDIO), TAG_AUDIO::session_active, false);
    st["sample_rate"] = m_replica.getUint32(static_cast<uint8_t>(ComponentId::AUDIO), TAG_AUDIO::sample_rate, 32000);
    j["state"] = st;

    // LED State
    nlohmann::json led;
    led["mode"] = static_cast<int>(getU8Field(m_replica, static_cast<uint8_t>(ComponentId::LED), TAG_LED::mode, 0));
    {
        std::vector<uint8_t> raw;
        if (m_replica.getRawField(static_cast<uint8_t>(ComponentId::LED), TAG_LED::color, raw) && raw.size() >= sizeof(RgbColor)) {
            RgbColor c;
            std::memcpy(&c, raw.data(), sizeof(c));
            led["color"] = { {"r", c.r}, {"g", c.g}, {"b", c.b} };
        } else {
            led["color"] = { {"r", 0}, {"g", 0}, {"b", 0} };
        }
    }
    led["speed_ms"] = m_replica.getUint32(static_cast<uint8_t>(ComponentId::LED), TAG_LED::speed_ms, 500);
    j["led"] = led;

    // Music State
    auto media_state = static_cast<MediaPlaybackState>(
        getU8Field(m_replica, static_cast<uint8_t>(ComponentId::MEDIA), TAG_MEDIA::state, 0));
    std::string song_title = m_replica.getString(static_cast<uint8_t>(ComponentId::MEDIA), TAG_MEDIA::title);
    std::string song_artist = m_replica.getString(static_cast<uint8_t>(ComponentId::MEDIA), TAG_MEDIA::artist);
    std::string track_id = m_replica.getString(static_cast<uint8_t>(ComponentId::MEDIA), TAG_MEDIA::active_song_id);

    nlohmann::json track;
    track["id"] = track_id;
    track["title"] = song_title;
    track["artist"] = song_artist;
    track["duration"] = m_replica.getUint32(static_cast<uint8_t>(ComponentId::MEDIA), TAG_MEDIA::duration_ms, 0) / 1000;

    nlohmann::json music;
    music["state"] = mediaStateToString(media_state);
    music["current_track"] = track;
    music["position_ms"] = m_replica.getUint32(static_cast<uint8_t>(ComponentId::MEDIA), TAG_MEDIA::position_ms, 0);
    music["duration_ms"] = m_replica.getUint32(static_cast<uint8_t>(ComponentId::MEDIA), TAG_MEDIA::duration_ms, 0);
    music["seekable"] = m_replica.getBool(static_cast<uint8_t>(ComponentId::MEDIA), TAG_MEDIA::seekable, false);
    music["repeat_mode"] = m_replica.getUint32(static_cast<uint8_t>(ComponentId::MEDIA), TAG_MEDIA::repeat_mode, 0);
    music["autoplay"] = m_replica.getBool(static_cast<uint8_t>(ComponentId::MEDIA), TAG_MEDIA::autoplay_enabled, true);
    music["caching"] = m_replica.getBool(static_cast<uint8_t>(ComponentId::MEDIA), TAG_MEDIA::cache_downloads, false);
    j["music"] = music;

    // Bluetooth
    nlohmann::json bt;
    bt["connected"] = m_replica.getBool(static_cast<uint8_t>(ComponentId::BLUETOOTH), TAG_BLUETOOTH::connected, false);
    bt["device_name"] = m_replica.getString(static_cast<uint8_t>(ComponentId::BLUETOOTH), TAG_BLUETOOTH::device_name, "");
    j["bluetooth"] = bt;

    return j.dump();
}

void StarServer::broadcastText(const std::string& msg) {
    std::vector<websocketpp::connection_hdl> dashboard_snapshot;
    {
        std::lock_guard<std::mutex> lock(m_conn_mutex);
        dashboard_snapshot.assign(m_dashboard_clients.begin(), m_dashboard_clients.end());
    }
    websocketpp::lib::error_code ec;
    for (auto& hdl : dashboard_snapshot) {
        m_server.send(hdl, msg, websocketpp::frame::opcode::text, ec);
    }
}

void StarServer::broadcastSnapshot() {
    broadcastText(buildSnapshotJson());
}

void StarServer::sendBinary(const std::vector<uint8_t>& frame) {
    websocketpp::connection_hdl target_hdl;
    {
        std::lock_guard<std::mutex> lock(m_conn_mutex);
        if (!m_has_waveshare_conn) return;
        target_hdl = m_waveshare_hdl;
    }

    websocketpp::lib::error_code ec;
    m_server.send(target_hdl, frame.data(), frame.size(), websocketpp::frame::opcode::binary, ec);
    if (ec) {
        std::cerr << "[StarServer] sendBinary error: " << ec.message() << std::endl;
    }
}

bool StarServer::sendSetField(uint8_t comp_id, uint8_t field_tag, const uint8_t* val, uint8_t val_len) {
    if (!isWaveshareConnected()) return false;
    auto frame = StarProtocol::buildSetFieldCmdFrame(comp_id, field_tag, val, val_len);
    sendBinary(frame);
    return true;
}

bool StarServer::sendExecAction(uint8_t cmd_id, uint32_t nonce, uint32_t param, const std::string& data) {
    if (!isWaveshareConnected()) return false;
    auto frame = StarProtocol::buildExecActionCmdFrame(cmd_id, nonce, param, data.c_str());
    sendBinary(frame);
    return true;
}

} // namespace StarReplica
