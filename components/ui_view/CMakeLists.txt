if(CONFIG_DISPLAY_ENABLE)
    set(UI_SRCS
        "src/SysDbUiDataSource.cpp"
        "src/ui/DashboardScreen.cpp"
        "src/ui/AssistantScreen.cpp"
        "src/ui/widgets/BarStrip.cpp"
        "src/ui/widgets/Card.cpp"
        "src/ui/widgets/DetailSheet.cpp"
        "src/ui/widgets/EqualizerBars.cpp"
        "src/ui/widgets/GroupCarousel.cpp"
        "src/ui/widgets/HBar.cpp"
        "src/ui/widgets/IconButton.cpp"
        "src/ui/widgets/LedColorSheet.cpp"
        "src/ui/widgets/MusicPage.cpp"
        "src/ui/widgets/Sparkline.cpp"
        "src/ui/widgets/StatRow.cpp"
        "src/ui/widgets/TranscriptList.cpp"
    )

    idf_component_register(
        SRCS ${UI_SRCS}
        INCLUDE_DIRS "include" "src/ui" "src"
        REQUIRES core_sysdb media_player lvgl esp_lvgl_port
    )
else()
    idf_component_register()
endif()
