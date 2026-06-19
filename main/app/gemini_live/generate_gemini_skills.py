import json
import os
import sys

def should_generate(schema_path, output_h, output_cpp):
    if not os.path.exists(output_h) or not os.path.exists(output_cpp):
        return True
    
    schema_mtime = os.path.getmtime(schema_path)
    if os.path.getmtime(output_h) < schema_mtime or os.path.getmtime(output_cpp) < schema_mtime:
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
                    h_content += f"    bool {param} = false;\n"
        h_content += "};\n\n"

    h_content += """struct DecodedSkillCall {
    SkillType type = SkillType::UNKNOWN;
    char call_id[64] = {0};
    union {
"""
    for tool in tools:
        skill_name = tool["name"]
        h_content += f"        {skill_name}_args_t* {skill_name};\n"
    
    h_content += """    } args;

    DecodedSkillCall() { memset(&args, 0, sizeof(args)); }
    ~DecodedSkillCall();
};

// Safe PSRAM-bounded translation method
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

DecodedSkillCall::~DecodedSkillCall() {{
    switch(type) {{
"""
    for tool in tools:
        skill_name = tool["name"]
        cpp_content += f"        case SkillType::{skill_name.upper()}: delete args.{skill_name}; break;\n"
    cpp_content += """        default: break;
    }
}

bool decode_incoming_arguments(const char* func_name, JsonObjectConst args_obj, DecodedSkillCall& out_call) {
    if (!func_name || args_obj.isNull()) return false;
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
                    cpp_content += f"            if (item_{param}.is<int>()) out_call.args.{skill_name}->{param} = item_{param}.as<int>();\n"
                elif ptype in ('NUMBER', 'FLOAT', 'DOUBLE'):
                    cpp_content += f"            if (item_{param}.is<float>() || item_{param}.is<double>() || item_{param}.is<int>()) out_call.args.{skill_name}->{param} = item_{param}.as<float>();\n"
                elif ptype in ('BOOLEAN', 'BOOL'):
                    cpp_content += f"            if (item_{param}.is<bool>()) out_call.args.{skill_name}->{param} = item_{param}.as<bool>();\n"
                cpp_content += f"        }}\n"
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
