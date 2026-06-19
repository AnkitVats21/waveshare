# Project Context: Waveshare Audio Development Board Firmware

## Project Overview
This is a C++ firmware project for a Waveshare Audio Development Board, built using the **ESP-IDF (Espressif IoT Development Framework)**. It handles complex embedded real-time operations such as:
- **Audio Processing:** Capturing microphone audio, playback via speaker (ES8311 DAC), RTP streaming/transception, and an audio pipeline manager.
- **Hardware Abstraction (HAL):** Managing I2S, I2C, IO Expanders (TCA9555), and SD cards.
- **Connectivity:** Wi-Fi connection management and MQTT communication.
- **System Architecture:** A modular, object-oriented approach organized into `app`, `common`, `hal`, and `services`.

## System Architecture & State Management (SysDB-Lite)
The firmware has been migrated from a legacy queue-backed `EventBus` to a low-latency, reactive state-reactor pattern called **SysDB-Lite (`EmbeddedSysDb`)**:
- **Single Source of Truth:** System state is centralized in `EmbeddedSysDb` using a flat, trivially copyable POD structure (`SystemState`) that prevents heap allocation churn during snapshots.
- **16-Bit Component/Field Hierarchical Masking:** The 32-bit `ComponentMask` is divided into the upper 16 bits (category tags like `COMP::AUDIO`, `COMP::ASSISTANT`) and the lower 16 bits (fine-grained change flags like `BIT_AUDIO::SPEAKER_VOLUME`). This allows reactors to wake up and process only relevant updates.
- **Task Notifications:** State changes wake up listener threads (`ReactorTask` subclasses) instantly and synchronously via FreeRTOS task notification bits (`xTaskNotify`), eliminating periodic polling loops.

## Development Environment
- The project is configured with a `.devcontainer` for Dockerized ESP-IDF environment (or QEMU) in VS Code.
- It uses the ESP-IDF Component Manager (`idf_component.yml`) to manage dependencies like `esp_codec_dev`, `esp_io_expander_tca95xx_16bit`, `led_strip`, and `mqtt`.
- Configuration is handled via ESP-IDF's Kconfig system (`Kconfig.projbuild`).

## Audio Pipeline & Playback (Leaky Bucket)
- **Leaky Bucket Playback**: Rather than gating on arbitrary queue fill level thresholds, `SpeakerPlaybackTask` runs at a constant rate (every 5ms / 120 samples at 24kHz) using `vTaskDelayUntil()`. It drains `SPK_RX_BUF` directly and writes silence frames if empty to keep the I2S DMA clock active.
- **Priority Control**: Incoming 16kHz RTP stream frames are discarded when the Gemini assistant is speaking (`s.audio.assistant_speaking` is true) or RTP rx is disabled.
- **Wake Word Engine**: WakeNet is suspended during playback to save CPU cycles and automatically re-armed via `AudioService` upon turn completion or VAD silence timeouts.

## Gemini Live Protocol Integration
- **Zero-Allocation Audio Paths**: Decoded audio and scratch workspaces are pre-allocated statically in PSRAM (`MALLOC_CAP_SPIRAM`) during boot.
- **Single-Pass Base64 Decode**: Decodes Base64-encoded audio directly into the static scratch buffer using the ceiling limit to minimize decoding passes.
- **Robust Frame Assembly**: Assembles fragmented WebSocket segments in a PSRAM scratch space. If a frame exceeds the 96KB limit, it is safely dropped to prevent heap or frame corruption.
- **ArduinoJson v7**: Used for serializing setup handshakes, transmitting tool responses, and parsing control messages.
- **Multi-Tool-Call Execution**: Handled via `MpvCommandHandler`, which processes all tool requests sent in the `functionCalls` array and publishes commands to the `mpv/command` MQTT topic.

## Building and Running
As an ESP-IDF project, standard `idf.py` commands apply:
- **Configure the project:** `idf.py menuconfig`
- **Build the firmware:** `idf.py build`
- **Flash to the device:** `idf.py -p <PORT> flash`
- **Monitor serial output:** `idf.py -p <PORT> monitor`
- **Build, flash, and monitor together:** `idf.py -p <PORT> flash monitor`

## Development Conventions
- **Language:** C++ (with ESP-IDF C API integration).
- **RAM Optimization**: Tasks use optimized stack sizes (defined in `common/thread_config.h`) to conserve internal DRAM and allow TLS/AES cryptographic engines to allocate dynamic workspaces. Large structures are forced to PSRAM.
- **Logging:** Uses a custom `LogRouter` and `AppLogger` instead of plain ESP-IDF logging macros in some areas.
- **Build System:** CMake.
