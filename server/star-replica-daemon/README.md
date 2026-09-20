# STAR Replica Daemon

Authoritative companion daemon for the Waveshare ESP32-S3 STAR architecture.

## Overview
The **STAR Replica Daemon** acts as the Central Host / Cloud WebSocket Server and state gateway:
- **Connection model**: Waveshare ESP32-S3 connects *outbound* to this daemon at `ws://<host-ip>:8765/api/star/ws`. Works transparently through NAT, firewalls, and cloud environments (e.g. AWS Lightsail).
- **State synchronization**: Maintains a live, in-memory mirror (`ReplicaTable`) of the device's authoritative state via monotonic `WAL_BATCH` streaming and full state snapshots.
- **Remote control**: Dispatches `CMD_SET_FIELD` and `CMD_EXEC_ACTION` to the Waveshare through the device write-gate.
- **Audio & Bluetooth bridge**:
  - **Tribit Bluetooth**: Monitors BlueZ A2DP sink connection and synchronizes `bluetooth.connected`, `bluetooth.device_name`, `bluetooth.mac_address` to Waveshare.
  - **MPD Playback**: Dispatches playback actions to local MPD when `media.output_target == PI_BT` and streams position updates.

## Building & Running

### 1. Build
```bash
cmake -B build -S .
cmake --build build
```

### 2. Run on PC / AWS Lightsail (Simulation & Dashboard Mode)
```bash
./build/star-replica-daemon 8765
```

### 3. Interactive CLI Commands
- `status`: Show current connection status, head sequence, and synced fields.
- `vol <0-100>`: Change speaker volume on Waveshare via `CMD_SET_FIELD`.
- `bt on` / `bt off`: Simulate Tribit speaker connection/disconnection.
- `quit`: Clean shutdown.
