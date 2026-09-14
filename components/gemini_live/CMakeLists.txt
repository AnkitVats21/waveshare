idf_component_register(
    SRCS
        "src/AssistantService.cpp"
        "src/MediaCommandHandler.cpp"
        "src/DeviceCommandHandler.cpp"
        "src/GeminiAudioPump.cpp"
        "src/GeminiProtocol.cpp"
        "src/gemini_skills_generated.cpp"
    INCLUDE_DIRS
        "include"
        "include/gemini_live"
    REQUIRES
        core_sysdb
        audio_core
        media_player
        esp_websocket_client
        mbedtls
        freertos
        esp_system
        esp_timer
        log
        heap
)

if(NOT CMAKE_SCRIPT_MODE_FILE)
    set(GEN_H "${CMAKE_CURRENT_SOURCE_DIR}/include/gemini_live/gemini_skills_generated.h")
    set(GEN_CPP "${CMAKE_CURRENT_SOURCE_DIR}/src/gemini_skills_generated.cpp")
    set(SCHEMA_JSON "${CMAKE_CURRENT_SOURCE_DIR}/schema/gemini_skills_schema.json")
    set(GEN_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_gemini_skills.py")

    add_custom_command(
        OUTPUT "${GEN_CPP}" "${GEN_H}"
        COMMAND python "${GEN_SCRIPT}" "${SCHEMA_JSON}" "${GEN_H}" "${GEN_CPP}"
        DEPENDS "${SCHEMA_JSON}" "${GEN_SCRIPT}"
        COMMENT "Generating Gemini Skills C++ framework from JSON schema"
    )
endif()
