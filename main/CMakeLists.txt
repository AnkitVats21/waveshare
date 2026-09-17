set(srcs
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

            # HAL — Bluetooth Companion
            "hal/companion/BtPlayerUart.cpp"
            "hal/companion/BtPlayerI2s.cpp"

            # Application layer
            "app/AppController.cpp"
            "app/audio/AudioService.cpp"
            "app/audio/recording/WavPcmEncoder.cpp"
            "app/audio/recording/AudioRecorder.cpp"
            "app/mqtt/MqttService.cpp"
            "app/input/KeyService.cpp"

            # Event and LED Services
            "app/led/LedService.cpp"

            # Services
            "services/network/WifiService.cpp"
            "services/network/HttpFileServerService.cpp"
            "services/config_manager/config_manager.cpp"
            "services/config_manager/config_generated.cpp"
            "services/storage/StorageService.cpp"
            "services/storage/SysDbSyncReactor.cpp"
            "services/time/TimeSyncHelper.cpp"
            "services/alarm/AlarmService.cpp"
)

idf_component_register(
        SRCS "${srcs}"
        INCLUDE_DIRS
            "."
            # HAL roots
            "hal"
            "hal/io"
            "hal/led"
            "hal/storage"
            "hal/audio"
            "hal/network"
            "hal/companion"
            "hal/input"
            # App layers
            "app"
            "app/audio"
            "app/mqtt"
            "app/led"
            # Services / common
            "services"
            "services/network"
            "common"
)


