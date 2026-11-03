idf_component_register(
    SRCS
        "src/ToolRegistry.cpp"
        "src/skills_generated.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        log
)

# The board's own tools: declarations and typed arguments generated from
# schema/gemini_skills_schema.json.
if(NOT CMAKE_SCRIPT_MODE_FILE)
    set(GEN_H "${CMAKE_CURRENT_SOURCE_DIR}/include/tool_registry/skills_generated.h")
    set(GEN_CPP "${CMAKE_CURRENT_SOURCE_DIR}/src/skills_generated.cpp")
    set(SCHEMA_JSON "${CMAKE_CURRENT_SOURCE_DIR}/schema/gemini_skills_schema.json")
    set(GEN_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_gemini_skills.py")

    add_custom_command(
        OUTPUT "${GEN_CPP}" "${GEN_H}"
        COMMAND python3 "${GEN_SCRIPT}" "${SCHEMA_JSON}" "${GEN_H}" "${GEN_CPP}"
        DEPENDS "${SCHEMA_JSON}" "${GEN_SCRIPT}"
        COMMENT "Generating the tool declarations and argument decoders from the schema"
        VERBATIM
    )
endif()
