# The link to a LAN hub and the voice backend through it; built with
# CONFIG_WAVESHARE_VOICE_HUB (main Kconfig). The headers are always there
# (main's ControlChannel implements IHubRelay).
set(hub_srcs)
if(CONFIG_WAVESHARE_VOICE_HUB)
    set(hub_srcs "src/HubLink.cpp" "src/HubAgent.cpp" "src/HubMusic.cpp"
                 "src/UplinkFraming.cpp" "src/UplinkEncoder.cpp")
endif()

idf_component_register(
    SRCS ${hub_srcs}
    INCLUDE_DIRS
        "include"
    REQUIRES
        voice_session
        gemini_live
        media_player
        audio_core
        core_sysdb
        credentials
        mdns
        esp_app_format
        esp_wifi
        esp_netif
        esp_timer
        esp_hw_support
        freertos
        log
        heap
)
