idf_component_register(
    SRCS
        "src/EmbeddedSysDb.cpp"
        "src/WalRingBuffer.cpp"
        "src/SysDbCodec.cpp"
        "src/ReactorTask.cpp"
        "src/BufferManager.cpp"
        "src/LogRouter.cpp"
        "src/AsyncNetLogger.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        freertos
        esp_system
        esp_timer
        esp_netif
        lwip
        log
        heap
        esp_ringbuf
)

# Automated STAR schema compilation via starc
set(STARC_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/starc/starc.py")
set(STAR_SCHEMA "${CMAKE_CURRENT_SOURCE_DIR}/../../schema/sysdb.star")
set(GEN_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/include/core_sysdb/SystemState.generated.h")
set(GEN_CODEC "${CMAKE_CURRENT_SOURCE_DIR}/src/SysDbCodec.generated.cpp")

add_custom_command(
    OUTPUT "${GEN_HEADER}" "${GEN_CODEC}"
    COMMAND python3 "${STARC_SCRIPT}" "${STAR_SCHEMA}" --out-header "${GEN_HEADER}" --out-codec "${GEN_CODEC}"
    DEPENDS "${STAR_SCHEMA}" "${STARC_SCRIPT}"
    COMMENT "Compiling STAR schema sysdb.star via starc"
    VERBATIM
)
add_custom_target(starc_schema DEPENDS "${GEN_HEADER}" "${GEN_CODEC}")
add_dependencies(${COMPONENT_LIB} starc_schema)
