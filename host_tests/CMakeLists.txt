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
)

nanobind_add_module(waveshare_host
    bindings/module.cpp
    bindings/bind_sysdb.cpp
    bindings/bind_buffers.cpp
    bindings/bind_resampler.cpp
    ../components/core_sysdb/src/EmbeddedSysDb.cpp
    ../components/core_sysdb/src/WalRingBuffer.cpp
    ../components/core_sysdb/src/SysDbCodec.cpp
    ../components/core_sysdb/src/BufferManager.cpp
    ../components/core_sysdb/src/ReactorTask.cpp
    ../components/audio_core/src/Resampler.cpp
)

target_compile_options(waveshare_host PRIVATE -Wall -Wextra)
