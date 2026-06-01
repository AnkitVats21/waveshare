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

            # HAL — Network
            "hal/network/WifiManager.cpp"

            # Application layer
            "app/AppController.cpp"
            "app/SystemContext.cpp"
            "app/audio/AudioService.cpp"
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

            # Services
            "services/BufferManager.cpp"

            # Common utilities
            "common/LogRouter.cpp"
            "common/AsyncNetLogger.cpp"

            # Services
            "services/BufferManager.cpp"
)

if(CONFIG_VOICE_BACKEND_RTP)
    list(APPEND srcs
        "app/audio/RtpTaskBase.cpp"
        "app/audio/RtpStreamer.cpp"
        "app/audio/RtpReceiver.cpp"
    )
endif()

if(CONFIG_VOICE_BACKEND_GEMINI_LIVE)
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/app/gemini_live/gemini_skills_generated.cpp
               ${CMAKE_CURRENT_SOURCE_DIR}/app/gemini_live/gemini_skills_generated.h
        COMMAND python ${CMAKE_CURRENT_SOURCE_DIR}/app/gemini_live/generate_gemini_skills.py 
                       ${CMAKE_CURRENT_SOURCE_DIR}/app/gemini_live/gemini_skills_schema.json 
                       ${CMAKE_CURRENT_SOURCE_DIR}/app/gemini_live/gemini_skills_generated.h 
                       ${CMAKE_CURRENT_SOURCE_DIR}/app/gemini_live/gemini_skills_generated.cpp
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/app/gemini_live/gemini_skills_schema.json
                ${CMAKE_CURRENT_SOURCE_DIR}/app/gemini_live/generate_gemini_skills.py
        COMMENT "Generating Gemini Skills C++ framework from JSON schema"
    )

    list(APPEND srcs
        "app/gemini_live/GeminiLiveService.cpp"
        "app/gemini_live/GeminiAudioPumpTask.cpp"
        "app/gemini_live/GeminiProtocolTask.cpp"
        "app/gemini_live/gemini_skills_generated.cpp"
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
            "hal/interfaces"
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
            "common/events"
            # HAL interfaces
            "hal/interfaces"
            # Gemini Integration
            "app/gemini_live"
)
