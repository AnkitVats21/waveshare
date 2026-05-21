#!/usr/bin/env python3
import sys
import time
import socket
import struct
import threading
import array

# Ensure pyaudio is installed or give instructions
try:
    import pyaudio
except ImportError:
    print("Error: PyAudio is required to run this script.")
    print("Please install it using: pip install pyaudio")
    print("Or on Debian/Ubuntu: sudo apt-get install python3-pyaudio")
    sys.exit(1)

# Protocol Constants
SERVER_IP = "127.0.0.1"
SERVER_PORT = 5005

MSG_START = 0x01
MSG_STOP = 0x03

PAYLOAD_L16_MONO = 96
PAYLOAD_CN = 13

# Audio Configuration
SAMPLE_RATE = 16000
CHANNELS = 1
CHUNK_SIZE = 512  # 32ms per packet at 16kHz
FORMAT = pyaudio.paInt16

class RtpBidiTestClient:
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # Bind to port 0 (ephemeral port assigned by OS)
        self.sock.bind(("0.0.0.0", 0))
        self.local_port = self.sock.getsockname()[1]
        
        self.p = pyaudio.PyAudio()
        
        # State management flags
        self.is_running = True
        self.is_streaming = False
        
        self.ssrc = 12345678
        self.seq_num = 0
        self.timestamp = 0
        
        self.play_stream = None
        self.record_stream = None
        
        self.rx_thread = None
        self.tx_thread = None

    def start_session(self):
        # Send MsgStart (0x01 + client_id)
        client_id = b"waveshare-esp32"
        start_packet = struct.pack("!B", MSG_START) + client_id
        self.sock.sendto(start_packet, (SERVER_IP, SERVER_PORT))
        print(f" -> [Network] MsgStart (0x01) sent from local port {self.local_port}")

    def stop_session(self):
        # Send MsgStop (0x03)
        stop_packet = struct.pack("!B", MSG_STOP)
        self.sock.sendto(stop_packet, (SERVER_IP, SERVER_PORT))
        print(" -> [Network] MsgStop (0x03) sent to Go Server")

    def rx_loop(self):
        # Open PyAudio output stream
        self.play_stream = self.p.open(
            format=FORMAT,
            channels=CHANNELS,
            rate=SAMPLE_RATE,
            output=True,
            frames_per_buffer=CHUNK_SIZE
        )

        while self.is_running:
            try:
                self.sock.settimeout(0.1)
                data, addr = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            except Exception as e:
                if self.is_running:
                    print(f"\n[!] Socket receive error: {e}")
                break

            if not data:
                continue

            # 1. Parse packet: Control or RTP?
            is_rtp = (len(data) >= 12) and ((data[0] >> 6) == 2)

            if not is_rtp:
                msg_type = data[0]
                if msg_type == 0x05:
                    print("\n🟢 [Go Server] MsgAssistantStop (0x05): Turn Complete! (Ready to speak again)")
                elif msg_type == 0x06:
                    print("\n🔊 [Go Server] MsgBacklightOn (0x06): AI is speaking (SOLID GREEN)")
                elif msg_type == 0x07:
                    print("\n🤫 [Go Server] MsgBacklightOff (0x07): AI is silent (BREATHING PINK)")
                else:
                    print(f"\n❔ [Go Server] Unknown Control Message: {hex(msg_type)}")
            else:
                # RTP Audio Packet
                try:
                    version_byte, payload_type_byte, seq_num, timestamp, ssrc = struct.unpack('!BBHII', data[:12])
                    payload_type = payload_type_byte & 0x7F
                    payload = data[12:]

                    if payload_type == PAYLOAD_L16_MONO:
                        # Convert big-endian PCM (L16) to host (little) endian PCM for PyAudio
                        a = array.array('h', payload)
                        a.byteswap()
                        self.play_stream.write(a.tobytes(), exception_on_overflow=False)
                    elif payload_type == PAYLOAD_CN:
                        # Comfort noise keepalive, ignore
                        pass
                except Exception as e:
                    pass

        if self.play_stream:
            self.play_stream.stop_stream()
            self.play_stream.close()

    def tx_loop(self):
        self.record_stream = self.p.open(
            format=FORMAT,
            channels=CHANNELS,
            rate=SAMPLE_RATE,
            input=True,
            frames_per_buffer=CHUNK_SIZE
        )

        while self.is_running:
            if not self.is_streaming:
                time.sleep(0.05)
                continue
                
            try:
                raw_pcm = self.record_stream.read(CHUNK_SIZE, exception_on_overflow=False)
                if not raw_pcm or not self.is_streaming:
                    continue
                
                # Convert raw PCM (usually little-endian) to big-endian (L16)
                a = array.array('h', raw_pcm)
                a.byteswap()
                big_endian_pcm = a.tobytes()

                # Pack RTP Header
                header = struct.pack('!BBHII', 0x80, PAYLOAD_L16_MONO, self.seq_num, self.timestamp, self.ssrc)
                rtp_packet = header + big_endian_pcm

                # Send RTP packet to server
                self.sock.sendto(rtp_packet, (SERVER_IP, SERVER_PORT))

                self.seq_num = (self.seq_num + 1) & 0xFFFF
                self.timestamp = (self.timestamp + CHUNK_SIZE) & 0xFFFFFFFF
            except Exception as e:
                if self.is_running:
                    print(f"\n[!] Error in TX thread: {e}")
                break

        if self.record_stream:
            self.record_stream.stop_stream()
            self.record_stream.close()

    def start(self):
        # 1. Start RX Thread
        self.rx_thread = threading.Thread(target=self.rx_loop, daemon=True)
        self.rx_thread.start()

        # 2. Start TX Thread
        self.tx_thread = threading.Thread(target=self.tx_loop, daemon=True)
        self.tx_thread.start()

        # 3. Enter Menu Loop
        self.run_menu()

    def run_menu(self):
        print("\n" + "=" * 60)
        print("    🌟 Bidirectional Multi-Turn RTP Test Client Operational 🌟")
        print("=" * 60)
        print("  * PRESS [ENTER] to Toggle Speech (Start / Stop speaking)")
        print("  * TYPE 'q' and press [ENTER] to exit")
        print("=" * 60 + "\n")

        while self.is_running:
            try:
                cmd = input().strip().lower()
                if cmd == 'q':
                    break
                
                if not self.is_streaming:
                    self.is_streaming = True
                    self.start_session()
                    print("\n🎤 [STREAMING ACTIVE] Speak now! (Press [ENTER] to finish speaking...)")
                else:
                    self.is_streaming = False
                    self.stop_session()
                    print("\n🤫 [SILENCE DETECTED] Listening to response... (Press [ENTER] to interrupt / speak again)")
            except KeyboardInterrupt:
                break

        self.cleanup()

    def cleanup(self):
        print("\n[*] Stopping bidirectional RTP client...")
        self.is_running = False
        self.is_streaming = False
        
        if self.rx_thread:
            self.rx_thread.join(timeout=1.0)
        if self.tx_thread:
            self.tx_thread.join(timeout=1.0)
            
        self.stop_session()
        self.p.terminate()
        self.sock.close()
        print("[+] Terminated successfully.")

if __name__ == "__main__":
    client = RtpBidiTestClient()
    client.start()

