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

            # Application layer
            "app/AppController.cpp"
            "app/audio/AudioService.cpp"
            "app/audio/recording/WavPcmEncoder.cpp"
            "app/audio/recording/AudioRecorder.cpp"
            "app/input/KeyService.cpp"

            # Event and LED Services
            "app/led/LedService.cpp"

            # Services
            "services/network/WifiService.cpp"
            "services/network/CaptiveDnsServer.cpp"
            "services/network/HttpFileServerService.cpp"
            "services/network/StarWsClient.cpp"
            "services/config_manager/config_manager.cpp"
            "services/config_manager/config_generated.cpp"
            "services/storage/StorageService.cpp"
            "services/storage/SysDbSyncReactor.cpp"
            "services/time/TimeSyncHelper.cpp"
            "services/alarm/AlarmService.cpp"
)

if(CONFIG_DISPLAY_ENABLE)
    list(APPEND srcs "hal/display/LcdManager.cpp")
    set(DISPLAY_REQUIRES
        ui_view
        esp_lcd
        esp_lcd_ili9341
        atanisoft__esp_lcd_touch_xpt2046
        esp_lvgl_port
        lvgl
    )
endif()

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
            "hal/input"
            "hal/display"
            # App layers
            "app"
            "app/audio"
            "app/led"
            # Services / common
            "services"
            "services/network"
            "common"
        REQUIRES
            core_sysdb
            audio_core
            media_player
            gemini_live
            ${DISPLAY_REQUIRES}
            nvs_flash
            esp_wifi
            esp_netif
            esp_http_server
            fatfs
            esp_driver_sdmmc
            esp_driver_gpio
            esp_driver_spi
            esp_driver_i2c
            esp_driver_uart
            esp_driver_i2s
            espressif__led_strip
            espressif__esp_io_expander_tca95xx_16bit
            espressif__esp_codec_dev
            esp_system
            esp_timer
            freertos
            log
            heap
            esp_event
            app_update
            lwip
)


