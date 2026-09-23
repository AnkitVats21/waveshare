import json
import os
import sys

def should_generate(schema_path, output_h, output_cpp):
    if not os.path.exists(output_h) or not os.path.exists(output_cpp):
        return True
    
    # Regenerate when either the schema or this generator changed.
    src_mtime = max(os.path.getmtime(schema_path), os.path.getmtime(__file__))
    if os.path.getmtime(output_h) < src_mtime or os.path.getmtime(output_cpp) < src_mtime:
        return True
        
    return False

def generate_gemini_framework(schema_path, output_h, output_cpp):
    if not should_generate(schema_path, output_h, output_cpp):
        print(f"Skipping generation: {output_h} and {output_cpp} are up to date.")
        return

    print(f"Generating Gemini skills from {schema_path}...")
    with open(schema_path, 'r') as f:
        tools = json.load(f)

    # 1. Generate Header Body
    h_content = """#pragma once
#include <string>
#include <cmath>
#include <cstring>
#include <ArduinoJson.h>

namespace GeminiSkills {

// Raw Handshake Setup Payload compiled down to Flash memory
extern const char* const SETUP_HANDSHAKE_JSON;

enum class SkillType {
    UNKNOWN,
"""
    for tool in tools:
        skill = tool["name"]
        h_content += f"    {skill.upper()},\n"
    h_content += "};\n\n"

    # Generate specialized structure payloads
    for tool in tools:
        skill_name = tool["name"]
        h_content += f"struct {skill_name}_args_t {{\n"
        params = tool.get("parameters", {}).get("properties", {})
        if params:
            for param, props in params.items():
                ptype = props.get("type", "").upper()
                if ptype == 'STRING':
                    h_content += f"    std::string {param};\n"
                elif ptype in ('INTEGER', 'INT'):
                    h_content += f"    int {param} = 0;\n"
                elif ptype in ('NUMBER', 'FLOAT', 'DOUBLE'):
                    h_content += f"    float {param} = 0.0f;\n"
                elif ptype in ('BOOLEAN', 'BOOL'):
                    if param == 'enabled':
                        h_content += f"    bool {param} = true;\n"
                    else:
                        h_content += f"    bool {param} = false;\n"
        h_content += "};\n\n"

    h_content += """struct DecodedSkillCall {
    SkillType type = SkillType::UNKNOWN;
    char call_id[64] = {0};
    // Why decoding failed (unknown tool, missing required argument); nullptr on success.
    const char* error = nullptr;
    union {
"""
    for tool in tools:
        skill_name = tool["name"]
        h_content += f"        {skill_name}_args_t* {skill_name};\n"
    
    h_content += """    } args;

    DecodedSkillCall() { memset(&args, 0, sizeof(args)); }
    ~DecodedSkillCall() { reset(); }
    DecodedSkillCall(const DecodedSkillCall&) = delete;
    DecodedSkillCall& operator=(const DecodedSkillCall&) = delete;

    // Frees the decoded arguments and returns to the empty state.
    void reset();
};

// Decodes a functionCall into out_call (reset first). args_obj may be null for
// parameterless tools. Returns false with out_call.error set for an unknown
// tool or a missing required argument.
bool decode_incoming_arguments(const char* func_name, JsonObjectConst args_obj, DecodedSkillCall& out_call);

} // namespace GeminiSkills
"""

    # 2. Generate C++ Source Layout
    setup_native = {
        "setup": {
            "model": "models/gemini-3.1-flash-live-preview",
            "generationConfig": {
                "responseModalities": ["AUDIO"],
                "speechConfig": {
                    "voiceConfig": {
                        "prebuiltVoiceConfig": {
                            "voiceName": "Sulafat" 
                        }
                    }
                }
            },
            "tools": [
                { "functionDeclarations": tools,},
            ]
        }
    }

    raw_json_escaped = json.dumps(setup_native).replace('"', '\\"')

    cpp_content = f"""#include "gemini_skills_generated.h"
#include "common/AppLogger.h"

namespace GeminiSkills {{

const char* const SETUP_HANDSHAKE_JSON = "{raw_json_escaped}";

void DecodedSkillCall::reset() {{
    switch(type) {{
"""
    for tool in tools:
        skill_name = tool["name"]
        cpp_content += f"        case SkillType::{skill_name.upper()}: delete args.{skill_name}; break;\n"
    cpp_content += """        default: break;
    }
    type = SkillType::UNKNOWN;
    call_id[0] = '\\0';
    error = nullptr;
    memset(&args, 0, sizeof(args));
}

bool decode_incoming_arguments(const char* func_name, JsonObjectConst args_obj, DecodedSkillCall& out_call) {
    out_call.error = "Unknown tool";
    if (!func_name) return false;
"""
    
    first = True
    for tool in tools:
        skill_name = tool["name"]
        if first:
            cpp_content += f'    if (strcmp(func_name, "{skill_name}") == 0) {{\n'
            first = False
        else:
            cpp_content += f'    }} else if (strcmp(func_name, "{skill_name}") == 0) {{\n'
            
        cpp_content += f"        out_call.type = SkillType::{skill_name.upper()};\n"
        cpp_content += f"        out_call.args.{skill_name} = new {skill_name}_args_t();\n"
        
        params = tool.get("parameters", {}).get("properties", {})
        if params:
            for param, props in params.items():
                ptype = props.get("type", "").upper()
                cpp_content += f'        JsonVariantConst item_{param} = args_obj["{param}"];\n'
                cpp_content += f"        if (!item_{param}.isNull()) {{\n"
                if ptype == 'STRING':
                    cpp_content += f"            if (item_{param}.is<const char*>()) out_call.args.{skill_name}->{param} = item_{param}.as<const char*>();\n"
                elif ptype in ('INTEGER', 'INT'):
                    # The model sometimes sends whole numbers as 7.0.
                    cpp_content += f"            if (item_{param}.is<float>()) out_call.args.{skill_name}->{param} = static_cast<int>(lroundf(item_{param}.as<float>()));\n"
                elif ptype in ('NUMBER', 'FLOAT', 'DOUBLE'):
                    cpp_content += f"            if (item_{param}.is<float>() || item_{param}.is<double>() || item_{param}.is<int>()) out_call.args.{skill_name}->{param} = item_{param}.as<float>();\n"
                elif ptype in ('BOOLEAN', 'BOOL'):
                    cpp_content += f"            if (item_{param}.is<bool>()) out_call.args.{skill_name}->{param} = item_{param}.as<bool>();\n"
                cpp_content += f"        }}\n"
        for param in tool.get("parameters", {}).get("required", []):
            cpp_content += f'        if (args_obj["{param}"].isNull()) {{\n'
            cpp_content += f'            out_call.error = "Missing required argument \'{param}\'";\n'
            cpp_content += "            return false;\n"
            cpp_content += "        }\n"
        cpp_content += "        out_call.error = nullptr;\n"
        cpp_content += "        return true;\n"

    cpp_content += """    }
    return false;
}

} // namespace GeminiSkills
"""

    with open(output_h, 'w') as f: f.write(h_content)
    with open(output_cpp, 'w') as f: f.write(cpp_content)

if __name__ == "__main__":
    generate_gemini_framework(sys.argv[1], sys.argv[2], sys.argv[3])
