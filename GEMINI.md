# Project Context: Waveshare Audio Development Board Firmware

## Project Overview
This is a C++ firmware project for a Waveshare Audio Development Board, built using the **ESP-IDF (Espressif IoT Development Framework)**. It handles complex embedded operations such as:
- **Audio Processing:** Capturing audio via microphone, playback via speaker, RTP streaming and receiving, and an audio pipeline manager.
- **Hardware Abstraction (HAL):** Managing I2S, I2C, IO Expanders (TCA9555), and SD cards.
- **Connectivity:** Wi-Fi connection management and MQTT communication.
- **System Architecture:** Uses a modular, object-oriented approach with components organized into `app`, `common`, `hal`, and `services`. It leverages an `EventBus` for cross-component communication and structured application layers (`AppController`).

## Development Environment
- The project is configured with a `.devcontainer`, enabling consistent development using a Dockerized ESP-IDF environment (or QEMU for emulation) in VS Code.
- It uses the ESP-IDF Component Manager (`idf_component.yml`) to manage dependencies like `esp_codec_dev`, `esp_io_expander_tca95xx_16bit`, `led_strip`, and `mqtt`.
- Configuration is handled via ESP-IDF's Kconfig system (`Kconfig.projbuild`), which allows customization of GPIO pins, hardware services, Wi-Fi credentials, and MQTT settings.

## Building and Running
As an ESP-IDF project, standard `idf.py` commands apply:
- **Configure the project:** `idf.py menuconfig`
- **Build the firmware:** `idf.py build`
- **Flash to the device:** `idf.py -p <PORT> flash`
- **Monitor serial output:** `idf.py -p <PORT> monitor`
- **Build, flash, and monitor together:** `idf.py -p <PORT> flash monitor`

## Development Conventions
- **Language:** C++ (with ESP-IDF C API integration).
- **Architecture:** 
  - Uses Singleton patterns heavily for central managers (e.g., `AppController::getInstance()`, `Board::getInstance()`).
  - Strict separation of concerns: `hal/` for hardware interfaces, `app/` for application logic, `services/` for system-wide services, and `common/` for shared types and utilities.
- **Logging:** Uses a custom `LogRouter` and `AppLogger` instead of plain ESP-IDF logging macros in some areas.
- **Build System:** CMake, following standard ESP-IDF project structure.
