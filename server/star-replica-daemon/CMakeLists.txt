cmake_minimum_required(VERSION 3.15)
project(star_replica_daemon CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(
    include
    vendor
)

find_package(Threads REQUIRED)

add_executable(star-replica-daemon
    src/main.cpp
    src/StarServer.cpp
)

target_link_libraries(star-replica-daemon PRIVATE Threads::Threads)
