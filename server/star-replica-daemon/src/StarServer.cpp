#include "StarServer.h"
#include <iostream>

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

        m_server.set_reuse_addr(true);
        m_server.listen(port);
        m_server.start_accept();

        m_running = true;
        m_server_thread = std::thread([this]() {
            std::cout << "[StarServer] Listening on port for Waveshare client connections..." << std::endl;
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
            if (m_has_waveshare_conn) {
                websocketpp::lib::error_code ec;
                m_server.close(m_waveshare_hdl, websocketpp::close::status::going_away, "Server shutting down", ec);
                m_has_waveshare_conn = false;
            }
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
    std::lock_guard<std::mutex> lock(m_conn_mutex);
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
}

void StarServer::onClose(websocketpp::connection_hdl /*hdl*/) {
    std::lock_guard<std::mutex> lock(m_conn_mutex);
    m_has_waveshare_conn = false;
    std::cout << "[StarServer] Waveshare device disconnected." << std::endl;
}

void StarServer::onMessage(websocketpp::connection_hdl /*hdl*/, WsServer::message_ptr msg) {
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
