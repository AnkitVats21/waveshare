idf_component_register(
        SRCS
            # Entry point
            "main.cpp"

            # HAL — Board orchestration
            "hal/Board.cpp"

            # HAL — Audio
            "hal/audio/AudioHal.cpp"

            # HAL — I/O
            "hal/io/I2CBus.cpp"
            "hal/io/IoExpander.cpp"

            # HAL — Input
            "hal/input/ExpanderKeyInput.cpp"

            # HAL — LED
            "hal/led/LedStripManager.cpp"

            # HAL — Storage
            "hal/storage/SdCardManager.cpp"

            # HAL — Network
            "hal/network/WifiManager.cpp"

            # Application layer
            "app/AppController.cpp"
            "app/audio/AudioService.cpp"
            "app/audio/RtpTaskBase.cpp"
            "app/audio/RtpStreamer.cpp"
            "app/audio/RtpReceiver.cpp"
            "app/audio/AudioPipelineManager.cpp"
            "app/audio/MicCapture.cpp"
            "app/audio/SpeakerPlayback.cpp"
            "app/mqtt/MqttTask.cpp"
            "app/input/KeyService.cpp"

            # Wake Word Detection
            "app/wake_word/WakeWordDetector.cpp"

            # Event and LED Services
            "app/event/EventBus.cpp"
            "app/led/LedService.cpp"

            # Common utilities
            "common/LogRouter.cpp"
            "common/AsyncNetLogger.cpp"

        INCLUDE_DIRS
            "."
            # HAL roots
            "hal"
            "hal/io"
            "hal/led"
            "hal/storage"
            "hal/audio"
            "hal/network"
            # App layers
            "app"
            "app/audio"
            "app/mqtt"
            "app/wake_word"
            "app/led"
            "app/event"
            # Services / common
            "services"
            "common"
)
