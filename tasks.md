# Project Tasks & Roadmap

## MQTT Improvements
- [ ] **Protobuf Integration**: Replace the simple `key=value` string parsing with Protocol Buffers for more robust and efficient communication.
- [ ] **Command Response**: Implement a `device/waveshare/config/ack` topic to confirm when a configuration change has been successfully applied.
- [ ] **Status Reporting**: Periodically publish the current volume and gain settings to a `device/waveshare/status` topic.
- [ ] **Enhanced Dynamic Subscriptions**: Add a way to unsubscribe from topics or list current active subscriptions.

## Audio Pipeline
- [ ] **AEC/AFE Optimization**: Fine-tune the Acoustic Echo Cancellation parameters via MQTT commands.
- [ ] **Wake Word Sensitivity**: Allow runtime adjustment of wake-word detection sensitivity.

## System
- [ ] **NVS Persistence**: Save volume and gain settings to NVS so they persist across reboots.
