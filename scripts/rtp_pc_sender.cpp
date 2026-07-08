#include <iostream>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <signal.h>

// Colors for terminal output
#define RESET   "\033[0m"
#define RED     "\033[1;31m"
#define GREEN   "\033[1;32m"
#define YELLOW  "\033[1;33m"
#define BLUE    "\033[1;34m"
#define MAGENTA "\033[1;35m"
#define CYAN    "\033[1;36m"
#define BOLD    "\033[1m"

// Global running flag for clean shutdown
volatile bool running = true;
std::string loaded_module_id = "";

// Signal handler for clean exit
void signal_handler(int sig) {
    std::cout << "\n" << YELLOW << "[INFO] Shutdown signal received. Exiting..." << RESET << std::endl;
    running = false;
}

// Check if the virtual sink already exists
bool check_virtual_sink_exists() {
    FILE* pipe = popen("pactl list short sinks 2>/dev/null", "r");
    if (!pipe) return false;
    char buffer[256];
    bool found = false;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        if (strstr(buffer, "rtp_virtual_sink") != nullptr) {
            found = true;
            break;
        }
    }
    pclose(pipe);
    return found;
}

// Create virtual sink (null sink)
bool create_virtual_sink() {
    std::cout << BLUE << "[INFO] Creating virtual device 'rtp_virtual_sink'..." << RESET << std::endl;
    FILE* pipe = popen("pactl load-module module-null-sink sink_name=rtp_virtual_sink sink_properties=device.description=\"RTP_Virtual_Sink\" 2>/dev/null", "r");
    if (!pipe) {
        std::cerr << RED << "[ERROR] Failed to run pactl load-module command." << RESET << std::endl;
        return false;
    }
    char buffer[128];
    std::string output = "";
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }
    int status = pclose(pipe);
    if (status == 0 && !output.empty()) {
        // Strip newlines
        while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
            output.pop_back();
        }
        loaded_module_id = output;
        std::cout << GREEN << "[SUCCESS] Loaded virtual sink module with ID: " << loaded_module_id << RESET << std::endl;

        // Unmute and set volume to 100% to ensure capture is not silent
        int r = 0;
        r = std::system("pactl set-sink-mute rtp_virtual_sink false 2>/dev/null");
        r = std::system("pactl set-sink-volume rtp_virtual_sink 100% 2>/dev/null");
        r = std::system("pactl set-source-mute rtp_virtual_sink.monitor false 2>/dev/null");
        r = std::system("pactl set-source-volume rtp_virtual_sink.monitor 100% 2>/dev/null");
        (void)r;

        return true;
    }
    std::cerr << RED << "[ERROR] pactl load-module failed." << RESET << std::endl;
    return false;
}

// Cleanup virtual sink on exit
void cleanup_virtual_sink() {
    if (!loaded_module_id.empty()) {
        std::cout << BLUE << "[INFO] Unloading virtual sink module ID " << loaded_module_id << "..." << RESET << std::endl;
        std::string cmd = "pactl unload-module " + loaded_module_id + " 2>/dev/null";
        int res = std::system(cmd.c_str());
        (void)res;
        loaded_module_id = "";
    }
}

struct rtp_header {
    uint8_t version_padding_x_cc; // Version (2 bits), Padding (1 bit), Extension (1 bit), CSRC Count (4 bits)
    uint8_t marker_pt;            // Marker (1 bit), Payload Type (7 bits)
    uint16_t seq_num;             // Sequence number
    uint32_t timestamp;           // Timestamp
    uint32_t ssrc;                // SSRC
} __attribute__((packed));

int main(int argc, char* argv[]) {
    // Print a cool ASCII banner
    std::cout << MAGENTA << BOLD;
    std::cout << "  ██████╗ ████████╗██████╗     ██████╗  ██████╗    ███████╗███████╗███╗   ██╗██████╗ ███████╗██████╗ \n";
    std::cout << "  ██╔══██╗╚══██╔══╝██╔══██╗   ██╔═══██╗██╔════╝    ██╔════╝██╔════╝████╗  ██║██╔══██╗██╔════╝██╔══██╗\n";
    std::cout << "  ██████╔╝   ██║   ██████╔╝   ██║   ██║██║         ███████╗█████╗  ██╔██╗ ██║██║  ██║█████╗  ██████╔╝\n";
    std::cout << "  ██╔══██╗   ██║   ██╔═══╝    ██║   ██║██║         ╚════██║██╔══╝  ██║╚██╗██║██║  ██║██╔══╝  ██╔══██╗\n";
    std::cout << "  ██║  ██║   ██║   ██║        ╚██████╔╝╚██████╗    ███████║███████╗██║ ╚████║██████╔╝███████╗██║  ██║\n";
    std::cout << "  ╚═╝  ╚═╝   ╚═╝   ╚═╝         ╚═════╝  ╚═════╝    ╚══════╝╚══════╝╚═╝  ╚═══╝╚═════╝ ╚══════╝╚═╝  ╚═╝\n";
    std::cout << RESET << std::endl;

    std::cout << CYAN << BOLD << "================== PC AUDIO RTP STREAMER ==================" << RESET << std::endl;

    std::string ip = "192.168.1.23";
    int port = 5005;
    std::string source_device = "rtp_virtual_sink.monitor";

    if (argc > 1) {
        std::string arg1 = argv[1];
        if (arg1 == "-h" || arg1 == "--help") {
            std::cout << "Usage: " << argv[0] << " [TARGET_IP] [PORT] [SOURCE_DEVICE]" << std::endl;
            std::cout << "Defaults:" << std::endl;
            std::cout << "  TARGET_IP:     192.168.1.23" << std::endl;
            std::cout << "  PORT:          5005" << std::endl;
            std::cout << "  SOURCE_DEVICE: rtp_virtual_sink.monitor" << std::endl;
            std::cout << "\nNotes:" << std::endl;
            std::cout << "  To stream the default PC output/microphone, use 'default' as the SOURCE_DEVICE." << std::endl;
            std::cout << "  If 'rtp_virtual_sink.monitor' is used, a virtual sink will be created automatically" << std::endl;
            std::cout << "  and cleaned up upon exit." << std::endl;
            return 0;
        }
        ip = arg1;
    }
    if (argc > 2) {
        try {
            port = std::stoi(argv[2]);
        } catch (...) {
            std::cerr << RED << "[ERROR] Invalid port number: " << argv[2] << RESET << std::endl;
            return 1;
        }
    }
    if (argc > 3) {
        source_device = argv[3];
    }

    // Set up signal handling
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // If using the default virtual device, verify/create it
    bool created_by_us = false;
    if (source_device == "rtp_virtual_sink.monitor") {
        if (!check_virtual_sink_exists()) {
            if (create_virtual_sink()) {
                created_by_us = true;
            } else {
                std::cerr << RED << "[ERROR] Could not create virtual device. Falling back to default sink monitor." << RESET << std::endl;
                source_device = "default";
            }
        } else {
            std::cout << GREEN << "[INFO] Virtual device 'rtp_virtual_sink' already exists." << RESET << std::endl;
            // Unmute and set volume to 100% just in case it was muted/attenuated
            int r = 0;
            r = std::system("pactl set-sink-mute rtp_virtual_sink false 2>/dev/null");
            r = std::system("pactl set-sink-volume rtp_virtual_sink 100% 2>/dev/null");
            r = std::system("pactl set-source-mute rtp_virtual_sink.monitor false 2>/dev/null");
            r = std::system("pactl set-source-volume rtp_virtual_sink.monitor 100% 2>/dev/null");
            (void)r;
        }
    }

    // Create UDP Socket
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::cerr << RED << "[ERROR] Failed to create socket." << RESET << std::endl;
        if (created_by_us) cleanup_virtual_sink();
        return 1;
    }

    struct sockaddr_in dest_addr;
    std::memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &dest_addr.sin_addr) <= 0) {
        std::cerr << RED << "[ERROR] Invalid target IP address: " << ip << RESET << std::endl;
        close(sock);
        if (created_by_us) cleanup_virtual_sink();
        return 1;
    }

    // Setup PulseAudio simple API configurations
    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = 24000;
    ss.channels = 1;

    pa_buffer_attr attr;
    attr.maxlength = (uint32_t) -1;
    attr.tlength = (uint32_t) -1;
    attr.prebuf = (uint32_t) -1;
    attr.minreq = (uint32_t) -1;
    attr.fragsize = 960; // 480 samples * 2 bytes = 960 bytes (20ms @ 24kHz)

    const char* dev_name = (source_device == "default") ? NULL : source_device.c_str();

    std::cout << BLUE << "[INFO] Target IP: " << ip << ":" << port << RESET << std::endl;
    std::cout << BLUE << "[INFO] Format:    PCM S16LE, 24000Hz, Mono" << RESET << std::endl;
    std::cout << BLUE << "[INFO] Source:    " << (dev_name ? dev_name : "default (system recording input)") << RESET << std::endl;

    pa_simple* pa_stream = nullptr;
    int retries = 10;
    while (retries > 0 && running) {
        int pa_err = 0;
        pa_stream = pa_simple_new(
            NULL,
            "RTP_PC_Sender",
            PA_STREAM_RECORD,
            dev_name,
            "Record from PC Audio for RTP Stream",
            &ss,
            NULL,
            &attr,
            &pa_err
        );
        if (pa_stream) {
            break;
        }
        
        std::cout << YELLOW << "[WARN] Waiting for PulseAudio source to become available... (" << retries << " retries left)" << RESET << std::endl;
        usleep(200000); // 200ms
        retries--;
    }

    if (!pa_stream) {
        std::cerr << RED << "[ERROR] Failed to open PulseAudio simple device: " << (dev_name ? dev_name : "default") << RESET << std::endl;
        close(sock);
        if (created_by_us) cleanup_virtual_sink();
        return 1;
    }

    std::cout << GREEN << BOLD << "\n[STREAM] Recording started successfully. Streaming RTP audio..." << RESET << std::endl;
    if (source_device == "rtp_virtual_sink.monitor") {
        std::cout << CYAN << "--> Hint: Open 'Pavucontrol' or System Settings and route any app's playback to 'RTP_Virtual_Sink'" << RESET << std::endl;
    }
    std::cout << YELLOW << "Press Ctrl+C to stop streaming." << RESET << "\n" << std::endl;

    const int CHUNK_SAMPLES = 480; // 20ms @ 24kHz
    const int CHUNK_SIZE = CHUNK_SAMPLES * 2;
    std::vector<uint8_t> buffer(sizeof(rtp_header) + CHUNK_SIZE);
    uint8_t* payload_ptr = buffer.data() + sizeof(rtp_header);

    uint16_t seq_num = 0;
    uint32_t timestamp = 0;

    while (running) {
        int pa_err = 0;
        if (pa_simple_read(pa_stream, payload_ptr, CHUNK_SIZE, &pa_err) < 0) {
            std::cerr << RED << "[ERROR] pa_simple_read() failed: " << pa_strerror(pa_err) << RESET << std::endl;
            break;
        }

        // Build RTP Header
        rtp_header* hdr = reinterpret_cast<rtp_header*>(buffer.data());
        hdr->version_padding_x_cc = 0x80;
        hdr->marker_pt = 96;
        hdr->seq_num = htons(seq_num);
        hdr->timestamp = htonl(timestamp);
        hdr->ssrc = htonl(0x55667788);

        // Transmit UDP packet
        ssize_t sent = sendto(sock, buffer.data(), buffer.size(), 0,
                              (struct sockaddr*)&dest_addr, sizeof(dest_addr));
        if (sent < 0) {
            // Ignore sending errors (e.g. temporary network down) to keep streaming alive
        }

        seq_num++;
        timestamp += CHUNK_SAMPLES;
    }

    std::cout << BLUE << "[INFO] Stopping stream..." << RESET << std::endl;

    if (pa_stream) {
        pa_simple_free(pa_stream);
    }
    if (sock >= 0) {
        close(sock);
    }

    if (created_by_us) {
        cleanup_virtual_sink();
    }

    std::cout << GREEN << "[SUCCESS] Cleanup complete. Goodbye!" << RESET << std::endl;
    return 0;
}
