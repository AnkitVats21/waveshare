#pragma once

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include <thread>
#include <mutex>
#include <memory>
#include <vector>
#include <string>
#include <set>
#include "ReplicaTable.h"
#include "core_sysdb/StarProtocol.h"

namespace StarReplica {

using WsServer = websocketpp::server<websocketpp::config::asio>;

class StarServer {
public:
    explicit StarServer(ReplicaTable& replica);
    ~StarServer();

    bool start(uint16_t port = 8765);
    void stop();

    bool isWaveshareConnected() const;

    // Command dispatch to authoritative Waveshare device
    bool sendSetField(uint8_t comp_id, uint8_t field_tag, const uint8_t* val, uint8_t val_len);
    bool sendExecAction(uint8_t cmd_id, uint32_t nonce, uint32_t param, const std::string& data = "");

    // Dashboard broadcast & serialization
    void broadcastSnapshot();
    std::string buildSnapshotJson() const;

private:
    ReplicaTable& m_replica;
    WsServer m_server;
    std::thread m_server_thread;
    bool m_running = false;

    mutable std::mutex m_conn_mutex;
    websocketpp::connection_hdl m_waveshare_hdl;
    bool m_has_waveshare_conn = false;

    // Web dashboard clients
    std::set<websocketpp::connection_hdl, std::owner_less<websocketpp::connection_hdl>> m_dashboard_clients;

    // WebSocketPP event handlers
    void onOpen(websocketpp::connection_hdl hdl);
    void onClose(websocketpp::connection_hdl hdl);
    void onMessage(websocketpp::connection_hdl hdl, WsServer::message_ptr msg);
    void onHttp(websocketpp::connection_hdl hdl);

    void sendBinary(const std::vector<uint8_t>& frame);
    void broadcastText(const std::string& msg);
    void handleDashboardTextMessage(websocketpp::connection_hdl hdl, const std::string& text);
};

} // namespace StarReplica
