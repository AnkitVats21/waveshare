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

            # Application layer
            "app/AppController.cpp"
            "app/audio/AudioService.cpp"
            "app/audio/recording/OggOpusEncoder.cpp"
            "app/audio/recording/PolyphaseResampler.cpp"
            "app/audio/recording/AudioRecorder.cpp"
            "app/input/KeyService.cpp"

            # Event and LED Services
            "app/led/LedService.cpp"

            # Services
            "services/network/WifiService.cpp"
            "services/network/CaptiveDnsServer.cpp"
            "services/http/HttpService.cpp"
            "services/http/ControlChannel.cpp"
            "services/http/SystemInfo.cpp"
            "services/http/FlashUpload.cpp"
            "services/http/routes/WebRoutes.cpp"
            "services/http/routes/WifiRoutes.cpp"
            "services/http/routes/FileRoutes.cpp"
            "services/http/routes/AudioRoutes.cpp"
            "services/http/routes/MusicRoutes.cpp"
            "services/http/routes/SystemRoutes.cpp"
            "services/http/routes/ConfigRoutes.cpp"
            "services/http/routes/OtaRoutes.cpp"
            "services/http/routes/AlarmRoutes.cpp"
            "services/http/routes/AlertRoutes.cpp"
            "services/http/routes/TimeRoutes.cpp"
            "services/http/routes/AssistantRoutes.cpp"
            "services/http/routes/DbRoutes.cpp"
            "services/storage/SysDbSyncReactor.cpp"
            "services/storage/SystemDatabase.cpp"
            "services/storage/generated/SystemDb.generated.cpp"
            "services/time/TimeSyncHelper.cpp"
            "services/alarm/AlarmService.cpp"
            "services/alarm/AlarmRing.cpp"
            "services/alerts/AlertLibrary.cpp"
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
            sd_storage
            credentials
            nexus_db
            ${DISPLAY_REQUIRES}
            nvs_flash
            esp_wifi
            esp_netif
            esp_http_server
            http_server
            mdns
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



# system.ndb code generated from schema/db/system.star by starc
set(NDB_SYSTEM_SCHEMA "${CMAKE_CURRENT_SOURCE_DIR}/../schema/db/system.star")
set(NDB_GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/services/storage/generated")
add_custom_command(
    OUTPUT "${NDB_GEN_DIR}/SystemDb.generated.h" "${NDB_GEN_DIR}/SystemDb.generated.cpp"
    COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/../tools/starc/starc.py" db "${NDB_SYSTEM_SCHEMA}" --out-dir "${NDB_GEN_DIR}"
    DEPENDS "${NDB_SYSTEM_SCHEMA}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/starc/starc.py" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/starc/ndb_gen.py"
    COMMENT "Compiling nexus_db schema system.star via starc"
    VERBATIM
)
add_custom_target(ndb_system_schema DEPENDS "${NDB_GEN_DIR}/SystemDb.generated.h" "${NDB_GEN_DIR}/SystemDb.generated.cpp")
add_dependencies(${COMPONENT_LIB} ndb_system_schema)
