#include <iostream>
#include <string>
#include <sstream>
#include <chrono>
#include <thread>
#include <csignal>
#include "ReplicaTable.h"
#include "StarServer.h"
#include "MockInterfaces.h"
#include "core_sysdb/SystemState.h"

static std::atomic<bool> g_running{true};

void signalHandler(int /*signum*/) {
    g_running = false;
}

int main(int argc, char* argv[]) {
    uint16_t port = 8765;
    if (argc > 1) {
        port = static_cast<uint16_t>(std::stoi(argv[1]));
    }

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    std::cout << "========================================================\n"
              << "   STAR Replication Daemon (Host / Cloud Gateway)       \n"
              << "   Listening on ws://0.0.0.0:" << port << "/api/star/ws \n"
              << "========================================================\n" << std::endl;

    StarReplica::ReplicaTable replica;
    StarReplica::StarServer server(replica);
    StarReplica::MockBluetoothSupervisor bt;
    StarReplica::MockPlaybackController player;

    // 1. Wire Bluetooth supervisor to forward state changes to Waveshare
    bt.start([&server](const StarReplica::BluetoothDeviceState& state) {
        if (!server.isWaveshareConnected()) return;

        uint8_t conn_byte = state.connected ? 1 : 0;
        server.sendSetField(
            static_cast<uint8_t>(ComponentId::BLUETOOTH),
            TAG_BLUETOOTH::connected,
            &conn_byte, 1);

        server.sendSetField(
            static_cast<uint8_t>(ComponentId::BLUETOOTH),
            TAG_BLUETOOTH::device_name,
            reinterpret_cast<const uint8_t*>(state.device_name.c_str()),
            state.device_name.size());

        server.sendSetField(
            static_cast<uint8_t>(ComponentId::BLUETOOTH),
            TAG_BLUETOOTH::mac_address,
            reinterpret_cast<const uint8_t*>(state.mac_address.c_str()),
            state.mac_address.size());

        server.sendSetField(
            static_cast<uint8_t>(ComponentId::BLUETOOTH),
            TAG_BLUETOOTH::rssi,
            reinterpret_cast<const uint8_t*>(&state.rssi), 1);
    });

    // 2. Wire MPD controller
    player.init([&server](uint32_t pos_ms, uint32_t /*dur_ms*/) {
        if (!server.isWaveshareConnected()) return;
        server.sendSetField(
            static_cast<uint8_t>(ComponentId::MEDIA),
            TAG_MEDIA::position_ms,
            reinterpret_cast<const uint8_t*>(&pos_ms), sizeof(pos_ms));
    });

    // 3. Monitor state mutations from Waveshare
    replica.addChangeListener([&server, &player](uint8_t comp_id, uint8_t field_tag, const std::vector<uint8_t>& val) {
        server.broadcastSnapshot();

        if (comp_id == static_cast<uint8_t>(ComponentId::MEDIA) && field_tag == TAG_MEDIA::pending_command) {
            if (val.size() >= sizeof(MediaPendingCommand)) {
                MediaPendingCommand cmd{};
                std::memcpy(&cmd, val.data(), sizeof(cmd));
                std::cout << "[Daemon] Received MediaPendingCommand: cmd=" << (int)cmd.cmd 
                          << " param=" << cmd.param << " data=" << cmd.data << std::endl;
                
                switch (cmd.cmd) {
                    case MediaCmdId::PLAY:
                        player.handleCommand(StarReplica::PlaybackCmd::PLAY, cmd.param, cmd.data);
                        break;
                    case MediaCmdId::PAUSE:
                        player.handleCommand(StarReplica::PlaybackCmd::PAUSE, 0, "");
                        break;
                    case MediaCmdId::RESUME:
                        player.handleCommand(StarReplica::PlaybackCmd::RESUME, 0, "");
                        break;
                    case MediaCmdId::STOP:
                        player.handleCommand(StarReplica::PlaybackCmd::STOP, 0, "");
                        break;
                    case MediaCmdId::SEEK:
                        player.handleCommand(StarReplica::PlaybackCmd::SEEK, cmd.param, "");
                        break;
                    default:
                        break;
                }
            }
        }
    });

    if (!server.start(port)) {
        std::cerr << "Failed to start StarServer on port " << port << std::endl;
        return 1;
    }

    std::cout << "Server running. Type 'help' for commands, 'quit' to exit.\n" << std::endl;

    // Interactive CLI loop (non-blocking if stdin is a pipe/redirect)
    while (g_running) {
        std::cout << "star> " << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) {
            // EOF (e.g. background mode or piped input): sleep and wait for signal
            while (g_running) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            break;
        }

        if (line.empty()) continue;

        std::stringstream ss(line);
        std::string cmd;
        ss >> cmd;

        if (cmd == "quit" || cmd == "exit") {
            break;
        } else if (cmd == "status") {
            std::cout << "Connected: " << (server.isWaveshareConnected() ? "YES" : "NO") << "\n"
                      << "Head Seq:  " << replica.getHeadSeq() << "\n"
                      << "Fields:    " << replica.getFieldCount() << "\n"
                      << "Vol:       " << replica.getInt32(static_cast<uint8_t>(ComponentId::AUDIO), TAG_AUDIO::speaker_volume) << "\n"
                      << "BT Conn:   " << (replica.getBool(static_cast<uint8_t>(ComponentId::BLUETOOTH), TAG_BLUETOOTH::connected) ? "YES" : "NO") << "\n"
                      << "Song:      " << replica.getString(static_cast<uint8_t>(ComponentId::MEDIA), TAG_MEDIA::title) << std::endl;
        } else if (cmd == "vol") {
            int vol = 0;
            if (ss >> vol) {
                server.sendSetField(
                    static_cast<uint8_t>(ComponentId::AUDIO),
                    TAG_AUDIO::speaker_volume,
                    reinterpret_cast<const uint8_t*>(&vol), sizeof(vol));
                std::cout << "Sent volume change: " << vol << std::endl;
            } else {
                std::cout << "Usage: vol <0-100>" << std::endl;
            }
        } else if (cmd == "bt") {
            std::string sub;
            ss >> sub;
            if (sub == "on") {
                bt.simulateConnection(true);
            } else if (sub == "off") {
                bt.simulateConnection(false);
            } else {
                std::cout << "Usage: bt on | bt off" << std::endl;
            }
        } else if (cmd == "help") {
            std::cout << "Commands:\n"
                      << "  status       Display current replicated state\n"
                      << "  vol <level>  Send speaker volume change to Waveshare\n"
                      << "  bt on|off    Simulate Tribit Bluetooth speaker connection\n"
                      << "  quit         Shutdown server" << std::endl;
        } else {
            std::cout << "Unknown command: " << cmd << " (type 'help' for available commands)" << std::endl;
        }
    }

    std::cout << "\nShutting down STAR replica daemon..." << std::endl;
    server.stop();
    bt.stop();
    player.stop();
    std::cout << "Daemon stopped cleanly." << std::endl;

    return 0;
}
