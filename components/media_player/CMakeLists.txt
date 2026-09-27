idf_component_register(
    SRCS
        "src/NexusPlayer.cpp"
        "src/AudioEngine.cpp"
        "src/StorageManager.cpp"
        "src/StreamManager.cpp"
        "src/HttpClientStream.cpp"
        "src/InvidiousClient.cpp"
        "src/InvidiousInstanceResolver.cpp"
        "src/WebMOpusDecoder.cpp"
        "src/WebmSeek.cpp"
        "src/OggSeek.cpp"
        "src/OggOpusDecoderStrategy.cpp"
        "src/AudioDecoderFactory.cpp"
        "src/MusicPlaybackService.cpp"
        "src/CatalogDB.cpp"
        "generated/MusicDb.generated.cpp"
    INCLUDE_DIRS
        "include"
        "generated"
    REQUIRES
        core_sysdb
        audio_core
        sd_storage
        esp_http_client
        mbedtls
        freertos
        esp_system
        esp_timer
        esp_rom
        cjson
        log
        heap
        nexus_db
)

# music.ndb code generated from schema/db/music.star by starc
set(MUSIC_DB_SCHEMA "${CMAKE_CURRENT_SOURCE_DIR}/../../schema/db/music.star")
set(MUSIC_DB_GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated")
set(MUSIC_DB_GEN_FILES "${MUSIC_DB_GEN_DIR}/MusicDb.generated.h" "${MUSIC_DB_GEN_DIR}/MusicDb.generated.cpp")
set(STARC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/starc")
add_custom_command(
    OUTPUT ${MUSIC_DB_GEN_FILES}
    COMMAND python3 "${STARC_DIR}/starc.py" db "${MUSIC_DB_SCHEMA}" --out-dir "${MUSIC_DB_GEN_DIR}"
    DEPENDS "${MUSIC_DB_SCHEMA}" "${STARC_DIR}/starc.py" "${STARC_DIR}/ndb_gen.py"
    COMMENT "Compiling music.ndb schema via starc"
    VERBATIM
)
add_custom_target(ndb_music_schema DEPENDS ${MUSIC_DB_GEN_FILES})
add_dependencies(${COMPONENT_LIB} ndb_music_schema)
