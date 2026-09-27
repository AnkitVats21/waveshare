cmake_minimum_required(VERSION 3.20)
project(waveshare_host_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Python 3.10 COMPONENTS Interpreter Development.Module REQUIRED)

execute_process(
    COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE OUTPUT_VARIABLE nanobind_DIR)
list(APPEND CMAKE_PREFIX_PATH "${nanobind_DIR}")

find_package(nanobind CONFIG REQUIRED)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/shims
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/core_sysdb/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/audio_core/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/nexus_db/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/media_player/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

# nexus_db code generated from the test schema by starc
set(NDB_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(NDB_STARC "${CMAKE_CURRENT_SOURCE_DIR}/../tools/starc/starc.py")
set(NDB_TEST_SCHEMA "${CMAKE_CURRENT_SOURCE_DIR}/schema/testdb.star")
add_custom_command(
    OUTPUT "${NDB_GEN_DIR}/TestdbDb.generated.h" "${NDB_GEN_DIR}/TestdbDb.generated.cpp" "${NDB_GEN_DIR}/ndb_schema.js"
    COMMAND python3 "${NDB_STARC}" db "${NDB_TEST_SCHEMA}" --out-dir "${NDB_GEN_DIR}" --js "${NDB_GEN_DIR}/ndb_schema.js"
    DEPENDS "${NDB_TEST_SCHEMA}" "${NDB_STARC}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/starc/ndb_gen.py"
    VERBATIM
)

nanobind_add_module(waveshare_host
    bindings/module.cpp
    bindings/bind_sysdb.cpp
    bindings/bind_buffers.cpp
    bindings/bind_resampler.cpp
    ../components/core_sysdb/src/EmbeddedSysDb.cpp
    ../components/core_sysdb/src/SysDbCodec.cpp
    ../components/core_sysdb/src/BufferManager.cpp
    ../components/core_sysdb/src/ReactorTask.cpp
    ../components/audio_core/src/Resampler.cpp
    bindings/bind_alerts.cpp
    ../components/audio_core/src/AlertMixer.cpp
    ../components/audio_core/src/AlertTones.cpp
    bindings/bind_alarm.cpp
    ../main/services/alarm/AlarmRing.cpp
    ../main/services/alarm/AlarmSchedule.cpp
    bindings/bind_nexus_db.cpp
    ../components/nexus_db/src/Database.cpp
    ../components/nexus_db/src/Crc32.cpp
    ${NDB_GEN_DIR}/TestdbDb.generated.cpp
    bindings/bind_media.cpp
    ../components/media_player/src/WebmCues.cpp
)

target_compile_options(waveshare_host PRIVATE -Wall -Wextra)
