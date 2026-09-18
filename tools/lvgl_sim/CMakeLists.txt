cmake_minimum_required(VERSION 3.20)
project(lvgl_sim CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

set(LV_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE STRING "" FORCE)

# LVGL's own examples/demos pull in widgets we don't enable in lv_conf.h and
# aren't needed by this tool -- don't build them.
set(LV_CONF_BUILD_DISABLE_EXAMPLES ON)
set(LV_CONF_BUILD_DISABLE_DEMOS ON)

FetchContent_Declare(
    lvgl
    GIT_REPOSITORY https://github.com/lvgl/lvgl.git
    GIT_TAG v9.2.2
)
FetchContent_MakeAvailable(lvgl)
# LVGL's own CMake looks for lv_conf.h on the include path; expose this
# directory (which holds it) to the lvgl target.
target_include_directories(lvgl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

find_package(SDL2 REQUIRED)
find_package(Threads REQUIRED)

set(ARDUINOJSON_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../managed_components/bblanchon__arduinojson")

set(SHARED_UI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../components/ui_view/src/ui")

add_executable(lvgl_sim
    main.cpp
    src/MiniHttpClient.cpp
    src/HttpUiDataSource.cpp
    ${SHARED_UI_DIR}/DashboardScreen.cpp
    ${SHARED_UI_DIR}/AssistantScreen.cpp
    ${SHARED_UI_DIR}/widgets/IconButton.cpp
    ${SHARED_UI_DIR}/widgets/BarStrip.cpp
    ${SHARED_UI_DIR}/widgets/EqualizerBars.cpp
    ${SHARED_UI_DIR}/widgets/Sparkline.cpp
    ${SHARED_UI_DIR}/widgets/HBar.cpp
    ${SHARED_UI_DIR}/widgets/Card.cpp
    ${SHARED_UI_DIR}/widgets/StatRow.cpp
    ${SHARED_UI_DIR}/widgets/GroupCarousel.cpp
    ${SHARED_UI_DIR}/widgets/MusicPage.cpp
    ${SHARED_UI_DIR}/widgets/LedColorSheet.cpp
    ${SHARED_UI_DIR}/widgets/DetailSheet.cpp
    ${SHARED_UI_DIR}/widgets/TranscriptList.cpp
)

target_include_directories(lvgl_sim PRIVATE
    src
    ${SHARED_UI_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ui_view/src
    ${ARDUINOJSON_ROOT}
    ${ARDUINOJSON_ROOT}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ui_view/include
)

target_link_libraries(lvgl_sim PRIVATE
    lvgl
    SDL2::SDL2
    Threads::Threads
)
