#include "StarServer.h"
#include <iostream>
#include <chrono>
#include <nlohmann/json.hpp>
#include "core_sysdb/SystemState.h"

namespace StarReplica {

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
        {
            std::lock_guard<std::mutex> lock(m_conn_mutex);
            websocketpp::lib::error_code ec;
            if (m_has_waveshare_conn) {
                m_server.close(m_waveshare_hdl, websocketpp::close::status::going_away, "Server shutting down", ec);
                m_has_waveshare_conn = false;
            }
            for (auto& hdl : m_dashboard_clients) {
                m_server.close(hdl, websocketpp::close::status::going_away, "Server shutting down", ec);
            }
            m_dashboard_clients.clear();
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

    std::lock_guard<std::mutex> lock(m_conn_mutex);
    if (path == "/api/star/ws") {
        m_waveshare_hdl = hdl;
        m_has_waveshare_conn = true;
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
        for (auto& w_hdl : m_dashboard_clients) {
            m_server.send(w_hdl, notif, websocketpp::frame::opcode::text, ec);
        }
    } else {
        m_dashboard_clients.insert(hdl);
        std::cout << "[StarServer] Web dashboard client connected (resource=" << path << ")" << std::endl;
        websocketpp::lib::error_code ec;
        m_server.send(hdl, buildSnapshotJson(), websocketpp::frame::opcode::text, ec);
    }
}

void StarServer::onClose(websocketpp::connection_hdl hdl) {
    std::lock_guard<std::mutex> lock(m_conn_mutex);
    if (m_has_waveshare_conn && !m_waveshare_hdl.owner_before(hdl) && !hdl.owner_before(m_waveshare_hdl)) {
        m_has_waveshare_conn = false;
        std::cout << "[StarServer] Waveshare device disconnected." << std::endl;

        std::string notif = R"({"type":"device_status","connected":false})";
        websocketpp::lib::error_code ec;
        for (auto& w_hdl : m_dashboard_clients) {
            m_server.send(w_hdl, notif, websocketpp::frame::opcode::text, ec);
        }
    } else {
        m_dashboard_clients.erase(hdl);
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

void StarServer::handleDashboardTextMessage(websocketpp::connection_hdl /*hdl*/, const std::string& text) {
    try {
        auto j = nlohmann::json::parse(text);
        std::string cmd = j.value("cmd", "");
        if (cmd == "volume") {
            int vol = j.value("value", 80);
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
            } else if (action == "seek") {
                uint32_t val = j.value("value", 0);
                sendExecAction(static_cast<uint8_t>(MediaCmdId::SEEK), 0, val, "");
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
        con->append_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        con->append_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
        return;
    }

    con->append_header("Access-Control-Allow-Origin", "*");
    con->append_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    con->append_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
    con->append_header("Content-Type", "application/json");

    if (method == "GET" && (path == "/api/system/delta" || path == "/api/system/init" || path == "/api/status")) {
        con->set_status(websocketpp::http::status_code::ok);
        con->set_body(buildSnapshotJson());
        return;
    }

    if (method == "GET" && path == "/api/music/status") {
        nlohmann::json j;
        std::string song_title = m_replica.getString(static_cast<uint8_t>(ComponentId::MEDIA), TAG_MEDIA::title);
        j["state"] = song_title.empty() ? "IDLE" : "STREAMING";
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

    if (method == "POST" && path == "/api/audio/volume") {
        try {
            auto body = nlohmann::json::parse(con->get_request_body());
            if (body.contains("volume")) {
                int vol = body["volume"].get<int>();
                sendSetField(static_cast<uint8_t>(ComponentId::AUDIO), TAG_AUDIO::speaker_volume,
                             reinterpret_cast<const uint8_t*>(&vol), sizeof(vol));
                con->set_status(websocketpp::http::status_code::ok);
                con->set_body(R"({"status":"ok","volume":)" + std::to_string(vol) + "}");
                return;
            }
        } catch (...) {}
    }

    if (method == "POST" && path == "/api/audio/mic_gain") {
        try {
            auto body = nlohmann::json::parse(con->get_request_body());
            if (body.contains("gain")) {
                float gain = body["gain"].get<float>();
                sendSetField(static_cast<uint8_t>(ComponentId::AUDIO), TAG_AUDIO::mic_gain_db,
                             reinterpret_cast<const uint8_t*>(&gain), sizeof(gain));
                con->set_status(websocketpp::http::status_code::ok);
                con->set_body(R"({"status":"ok"})");
                return;
            }
        } catch (...) {}
    }

    if (method == "POST" && path == "/api/audio/mic_mute") {
        try {
            auto body = nlohmann::json::parse(con->get_request_body());
            if (body.contains("muted")) {
                bool muted = body["muted"].get<bool>();
                uint8_t enabled = muted ? 0 : 1;
                sendSetField(static_cast<uint8_t>(ComponentId::AUDIO), TAG_AUDIO::mic_enabled, &enabled, 1);
                con->set_status(websocketpp::http::status_code::ok);
                con->set_body(R"({"status":"ok"})");
                return;
            }
        } catch (...) {}
    }

    if (method == "POST" && path == "/api/music/control") {
        try {
            auto body = nlohmann::json::parse(con->get_request_body());
            std::string action = body.value("action", "");
            if (action == "pause") {
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
                uint32_t val = body.value("value", 0);
                sendExecAction(static_cast<uint8_t>(MediaCmdId::SEEK), 0, val, "");
            }
            con->set_status(websocketpp::http::status_code::ok);
            con->set_body(R"({"status":"ok"})");
            return;
        } catch (...) {}
    }

    if (method == "POST" && path == "/api/music/play") {
        try {
            auto body = nlohmann::json::parse(con->get_request_body());
            std::string stream_url = body.value("stream_url", "");
            sendExecAction(static_cast<uint8_t>(MediaCmdId::PLAY), 0, 0, stream_url);
            con->set_status(websocketpp::http::status_code::ok);
            con->set_body(R"({"status":"ok"})");
            return;
        } catch (...) {}
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
    j["c0"] = 0;
    j["c1"] = 0;
    j["sram"] = 120000;
    j["min_sram"] = 90000;
    j["psram"] = 4194304;
    j["rssi"] = -48;
    j["latest_seq"] = m_replica.getHeadSeq();

    // Hardware State
    nlohmann::json st;
    st["speaker_volume"] = m_replica.getInt32(static_cast<uint8_t>(ComponentId::AUDIO), TAG_AUDIO::speaker_volume, 80);
    st["mic_gain_db"] = m_replica.getFloat(static_cast<uint8_t>(ComponentId::AUDIO), TAG_AUDIO::mic_gain_db, 60.0f);
    st["mic_enabled"] = m_replica.getBool(static_cast<uint8_t>(ComponentId::AUDIO), TAG_AUDIO::mic_enabled, true);
    st["is_recording"] = false;
    st["sample_rate"] = 32000;
    j["state"] = st;

    // Music State
    std::string song_title = m_replica.getString(static_cast<uint8_t>(ComponentId::MEDIA), TAG_MEDIA::title);
    std::string song_artist = m_replica.getString(static_cast<uint8_t>(ComponentId::MEDIA), TAG_MEDIA::artist);
    std::string track_id = m_replica.getString(static_cast<uint8_t>(ComponentId::MEDIA), TAG_MEDIA::active_song_id);

    nlohmann::json track;
    track["id"] = track_id;
    track["title"] = song_title;
    track["artist"] = song_artist;
    track["duration"] = m_replica.getUint32(static_cast<uint8_t>(ComponentId::MEDIA), TAG_MEDIA::duration_ms, 0) / 1000;

    nlohmann::json music;
    music["state"] = song_title.empty() ? "IDLE" : "PLAYING";
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
    std::lock_guard<std::mutex> lock(m_conn_mutex);
    websocketpp::lib::error_code ec;
    for (auto& hdl : m_dashboard_clients) {
        m_server.send(hdl, msg, websocketpp::frame::opcode::text, ec);
    }
}

void StarServer::broadcastSnapshot() {
    broadcastText(buildSnapshotJson());
}

void StarServer::sendBinary(const std::vector<uint8_t>& frame) {
    std::lock_guard<std::mutex> lock(m_conn_mutex);
    if (!m_has_waveshare_conn) return;

    websocketpp::lib::error_code ec;
    m_server.send(m_waveshare_hdl, frame.data(), frame.size(), websocketpp::frame::opcode::binary, ec);
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

