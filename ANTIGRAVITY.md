# 🛰️ ANTIGRAVITY.md — Waveshare ESP32-S3 Audio Firmware Reference

Welcome to the ultimate system reference manual for the **Waveshare Audio Development Board Firmware**. This file is a self-generated, live reference cataloging the architecture, HAL structures, communications protocol, state caches, events, and development roadmap of this codebase. It is designed to maintain high alignment between Antigravity (the coding assistant) and the developer during pair programming.

---

## 🛠️ 1. Project Context & Environment
* **Platform:** ESP32-S3 (Dual-core Xtensa LX7, high-speed PSRAM enabled, Wi-Fi 2.4GHz + BLE).
* **Framework:** **ESP-IDF v6.0.1** (Espressif IoT Development Framework) using C++ OOP conventions and CMake.
* **Component Dependencies (`idf_component.yml`):**
  * `espressif/esp_codec_dev: ^1.5.1` (Abstraction for audio codec devices).
  * `espressif/esp_io_expander_tca95xx_16bit: ^2.0.1` (TCA9555 IO expander driver).
  * `espressif/led_strip: ^2.4.1` (RGB WS2812/NeoPixel controller).
  * `espressif/mqtt` (Espressif native MQTT engine).
  * `espressif/esp-sr` (Espressif speech recognition framework for wake-word detection).
* **Compiling & Building Tooling:**
  * Configured via `eim_config.toml` to install and activate `v6.0.1` toolchains (xtensa-esp-elf, riscv32-esp-elf, cmake, ninja, openocd).
  * Development occurs inside a dedicated `.devcontainer` wrapper for absolute environment lock-in.

---

## 🏗️ 2. Architectural Structure
The codebase strictly separates concerns into a modular, object-oriented layering:

```
                  ┌──────────────────────────────────────────────┐
                  │                 app_main()                   │
                  └──────────────────────┬───────────────────────┘
                                         ▼
                  ┌──────────────────────────────────────────────┐
                  │                AppController                 │
                  └──────┬───────────────┬───────────────┬───────┘
                         │               │               │
                         ▼               ▼               ▼
                  ┌────────────┐   ┌────────────┐   ┌────────────┐
                  │AudioService│   │  MqttTask  │   │ LedService │
                  └──────┬─────┘   └────────────┘   └────────────┘
                         │
                         ▼
  ┌─────────────────────────────────────────────────────────────────────────────┐
  │                            EventBus (ESP Event Loop)                        │
  └──────────────────────────────────────┬──────────────────────────────────────┘
                                         │
                                         ▼
  ┌─────────────────────────────────────────────────────────────────────────────┐
  │                               Board (HAL Hub)                               │
  └──────┬───────────────┬───────────────┬───────────────┬───────────────┬──────┘
         │               │               │               │               │
         ▼               ▼               ▼               ▼               ▼
   ┌──────────┐    ┌──────────┐    ┌──────────┐    ┌──────────┐    ┌──────────┐
   │ AudioHal │    │  I2CBus  │    │IoExpander│    │ LedStrip │    │SdCardMgr │
   └──────────┘    └──────────┘    └──────────┘    └──────────┘    └──────────┘
```

### 📂 Directory Mapping:
1. **`main/`**: Contain app entry point (`main.cpp`) and component registries.
2. **`main/app/`**: Application orchestrator, dynamic services, and runtime protocols.
   * `AppController`: System bootstrap controller, monitors Wi-Fi connection states, hooks up network log routing, and triggers subsystem startups.
   * `audio/`: Handles RTP streaming (`RtpStreamer`), RTP playback (`RtpReceiver`, `SpeakerPlayback`), and pipeline assembly (`AudioPipelineManager`, `AudioService`, `MicCapture`).
   * `event/`: Central messaging channel wrapper (`EventBus`).
   * `input/`: High-level button services (`KeyService`).
   * `led/`: RGB LED animation logic and color handlers (`LedService`).
   * `mqtt/`: Asynchronous secure telemetry and remote command processing (`MqttTask`).
   * `wake_word/`: Local high-accuracy microphone speech processing triggers (`WakeWordDetector`).
3. **`main/common/`**: Low-overhead logging components, base-classes, and structure types.
   * `AppLogger.h`: Structured macros for logging tags (`LOGI_SYSTEM`, `LOGE_AUDIO`, etc.).
   * `LogRouter`: Flexible multiplexing of console output to remote UDP servers (`AsyncNetLogger`).
   * `TaskBase.h`: Custom OOP wrapper over standard FreeRTOS tasks.
   * `app_types.h`: Central configuration structs, hardware descriptors, and event lists.
4. **`main/hal/`**: Hardware Abstraction Layer, interfaces with physical chips.
   * `Board`: Orchestrator of peripheral initialization (I2C, expanders, NVS, storage, audio codecs).
   * `audio/`: Low-level I2S peripheral config and ES8311 driver management (`AudioHal`).
   * `io/`: TCA9555 16-bit IO expander interface (`IoExpander`) and dynamic I2C bus driver (`I2CBus`).
   * `input/`: Abstraction of buttons hooked to the IO expander (`ExpanderKeyInput`).
   * `led/`: Direct led-strip driver interface (`LedStripManager`).
   * `storage/`: MicroSD file system mount/unmount operations (`SdCardManager`).
5. **`main/services/`**: Supporting services (`config_manager` for system states).

---

## 🔌 3. Hardware Integration & HAL Configuration

### 📌 Core Pin Mappings (Configured via `Kconfig.projbuild`):
* **I2C Bus:** SCL = `GPIO 10`, SDA = `GPIO 11` (Standard rate: `100 kHz`).
* **I2S Audio Bus:** MCLK = `GPIO 12`, BCLK = `GPIO 13`, LRCK = `GPIO 14`, DOUT = `GPIO 16`, DIN = `GPIO 15`.
* **RGB LED Pin:** `GPIO 38` (WS2812 strip).
* **SDMMC Storage:** CLK = `GPIO 40`, D0 = `GPIO 41`, CMD = `GPIO 42`.
* **IO Expander Pins (TCA9555 Address `0x20`):**
  * SD Card Detect (`SD_CD`): EXIO Index `3` (Active low).
  * Power Amp Enable (`PA_EN`): EXIO Index `8` (Active high).
  * Board Buttons: KEY1 = EXIO Index `9`, KEY2 = EXIO Index `10`, KEY3 = EXIO Index `11` (Active low).

### 🔊 Audio Peripherals:
* **Playback Codec:** ES8311 (I2C address: `0x18`, standard `16-bit PCM` audio output, driving a single physical speaker over mono channel).
* **Microphone Feed:** Driven via I2S RX channel capturing high-fidelity samples for Wake-Word processing (AFE) and voice streaming.

---

## 📡 4. System Events Directory (`app_types.h`)
System modules are completely decoupled using the `EventBus` (which wraps native `esp_event` loops via a C++ singleton template).

### 🏷️ Declared Event Bases:
* `AUDIO_SYSTEM_EVENTS` (Internal audio driver states)
* `WIFI_SYSTEM_EVENTS` (Network connect/disconnect updates)
* `MQTT_SYSTEM_EVENTS` (MQTT command and subscription triggers)
* `APP_EVENTS` (High-level application directives)

### 📲 Unified Application Events (`AppEvent`):
| Event Enum | Trigger Condition / Action |
|:---|:---|
| `WAKE_WORD_DETECTED` | Fired by `WakeWordDetector` when speech model triggers. |
| `STREAMING_STOP_REQUESTED` | Fired when stopping active audio streams. |
| `STOP_STREAMING` | Commands `AudioService` to close down RTP stream pipes. |
| `LED_COLOR_UPDATE` | Fired when LED color changes (e.g. from MQTT commands). |
| `MIC_GAIN_UPDATE` | Fired when microphone gain values are updated dynamically. |
| `LED_COMMAND` | Dispatches specific modes/colors to `LedService` animations. |
| `ASSISTANT_TALKING` | Indicates the smart assistant is streaming speaker data. |
| `ASSISTANT_SILENT` | Fired when speaker goes silent. |
| `ASSISTANT_TURN_COMPLETE` | Signifies the assistant voice output transaction is done. |
| `USER_INTERRUPTED` | Fired if button click or noise interrupts output. |

### 🌐 Network Events (`WifiEvent`):
* `CONNECTED`: Dispatched by `WifiManager` -> Boots up UDP remote logger -> Activates `AudioService` -> Initializes secure `MqttTask`.
* `DISCONNECTED`: Dispatched on connection loss -> Tears down network dependencies -> Blinks status LED red.

---

## ⚡ 5. State Cache & Telemetry System
`MqttTask` maintains a synchronized client state representation of the board within `m_cache`, aligning configuration updates from remote brokers:

```cpp
struct {
  int speaker_volume;     // Target Volume (0 - 100)
  float mic_volume;       // Mic Gain (db level or percentage)
  uint32_t sample_rate;   // Active audio playback rate (e.g., 16000Hz)
  bool mic_enabled;       // Microphone capture control flag
  struct { 
    uint8_t r, g, b; 
  } led_color;            // Cached RGB color values
} m_cache;
```

### 🛰️ MQTT Topic Interface:
* **Commands / Subscriptions:**
  * `device/esp32s3/commands`: General control events.
  * `device/waveshare/config`: Dynamic configuration updates (processes standard `key=value` string payloads, such as `speaker_volume=85` or `led_color=80,0,80`).
  * `device/subscribe/topic`: Allows brokers to dynamically register new subscriptions on the device during runtime.
* **Telemetry / Publications:**
  * `runtime/status`: Last Will and Testament (LWT) topic.
  * `device/esp32s3/telemetry`: Publishes periodic metrics (e.g. volume values, network logging metrics).

---

## 🚀 6. Subsystem Deep-Dive

### 🎙️ 6.1 Audio Pipeline
The pipeline manages incoming/outgoing high-rate PCM streams in real-time under tight FreeRTOS priorities (`audio_task_priority = 22`):
* **Capturing (`MicCapture`):** Retrieves 4-channel interleaved audio (`RMNM` format) -> Feeds to Speech Recognition / AFE -> Remaps to mono for streaming.
* **Streamer (`RtpStreamer`):** Packages mono microphone samples into low-latency RTP packets and transmits them to the server.
* **Receiver (`RtpReceiver`):** Pulls down inbound RTP packets from the network, strips 12-byte headers, and feeds them into the playback ringbuffer.
* **Playback (`SpeakerPlayback`):** Unpacks PCM frames from ringbuffer -> Scales based on volume settings -> Outputs to ES8311 playback codec.

### 🗣️ 6.2 Wake Word Detector (`WakeWordDetector`)
Uses the local `esp-sr` engine reading microphone hardware buffers on a dedicated FreeRTOS task. It is configured to run at boot. If a voice trigger matches the `model` partition (e.g., model name "hi_esp" configured above the `0.7` threshold), it dispatches a `WAKE_WORD_DETECTED` event on the `EventBus` to initiate voice streaming transactions.

### 🔘 6.3 IO Expander Key Polling (`KeyService` & `ExpanderKeyInput`)
An asynchronous high-rate poller monitors key pins on the TCA9555 expander. If state changes occur, they are debounced and mapped to internal keystrokes, feeding into application-layer controller interactions.

### 💡 6.4 LED Strip Manager (`LedService` & `LedStripManager`)
Drives RGB pixels on `GPIO 38` asynchronously to visually wow the user with fluid system transitions:
* **Green Solid:** WiFi Connected & Bootstrapping complete.
* **Red Solid / Blink:** System boot state or Wi-Fi Disconnected.
* **Blue Blink (2x):** Audio subsystem successfully bootstrapped.
* **Purple Blink (2x):** MQTT service running and securely connected.

---

## 🗺️ 7. Development Roadmap & Backlog

The following list merges outstanding items from `tasks.md` and optimization requirements from `refactor.md`:

### 📉 7.1 Mono Audio Path Optimization (Critical Performance Fix)
**Objective:** Replace the current heavy mono-to-stereo duplicate pipeline with an end-to-end true mono channel to resolve UDP packet loss and ringbuffer congestion warnings (`RX RingBuffer FULL`).

```
[Incoming Mono RTP] -> [Mono Ringbuffer] -> [Mono I2S Slot] -> [Mono Codec Write (ES8311)]
```

* **Step 1 (Board.cpp):** Change I2S slot configuration from stereo standard to mono:
  ```cpp
  // Replace I2S_SLOT_MODE_STEREO with:
  I2S_SLOT_MODE_MONO
  ```
* **Step 2 (Board.cpp):** Reconfigure codec output channel count:
  ```cpp
  fs.channel = 1; // Mono mode
  ```
* **Step 3 (SpeakerPlayback.cpp):** Eliminate local stereo expansion and allocations:
  * Remove `stereo_buffer` allocation.
  * Delete mono-to-stereo expansion loops copying left samples to right channels.
* **Step 4 (SpeakerPlayback.cpp):** Write Mono PCM frames directly:
  ```cpp
  esp_codec_dev_write(param->device, pcm_ptr, rx_chunk_bytes);
  ```
* **Step 5 (SpeakerPlayback.cpp):** Add timing instrumentation logs to measure codec blocking:
  ```cpp
  uint64_t start = esp_timer_get_time();
  // ... esp_codec_dev_write ...
  uint64_t end = esp_timer_get_time();
  LOGI_HAL("Codec write took %llu us", end - start);
  ```
* **Step 6 (rtp_sender.py):** Align packet duration to 20ms at 32kHz (increase `CHUNK_SAMPLES` to `640`) to decrease network interrupt rates and socket overhead.
* **Step 7 (Board.cpp):** Tune DMA configurations to improve buffer resilience under Wi-Fi traffic bursts:
  ```cpp
  dma_desc_num = 12;
  dma_frame_num = 512;
  ```

### 💬 7.2 MQTT Telemetry & Protocol Upgrades
* [ ] **Protobuf Integration:** Swap out fragile key-value string parsing (`key=val`) in `MqttTask::processIncomingData` for high-robustness **Protocol Buffers**.
* [ ] **Command Config Ack:** Implement transactional validation by publishing back configuration receipts on a dedicated `device/waveshare/config/ack` topic.
* [ ] **Status Reporting:** Establish periodic state publishing sending active volume, microphone gain, and Wi-Fi metrics to a `device/waveshare/status` topic.
* [ ] **Enhanced Dynamic Subscriptions:** Expand dynamic subscribe mechanisms to support explicit unsubscribing or live topic enumeration listing.

### 💾 7.3 System & Audio Tuning
* [ ] **NVS Persistence:** Store volume and mic gain configurations inside non-volatile storage (NVS) to preserve setting selections across hardware reboots.
* [ ] **AEC/AFE Optimization:** Enable fine-grained tweaking of Acoustic Echo Cancellation parameters over MQTT controls.
* [ ] **Wake Word Sensitivity:** Allow dynamic adjustment of detection threshold levels at runtime without model partition re-flashing.

---

## 🛠️ 8. Useful Commands (ESP-IDF v6.0.1)

Build, flash, and debug with the standard ESP-IDF commands:
```bash
# Configure the firmware features and pin parameters
idf.py menuconfig

# Run full project compilation
idf.py build

# Flash binary to your board (replaces <PORT> with /dev/ttyACM0 or similar)
idf.py -p <PORT> flash

# Monitor output logs and crash traces in real-time 
idf.py -p <PORT> monitor

# Combined compilation, flashing, and monitor command
idf.py -p <PORT> flash monitor

#if <port>  not passed then default port is selected
```
---
*Created automatically by Antigravity as a system context anchor.*
