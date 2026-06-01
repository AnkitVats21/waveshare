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
        schema = json.load(f)

    # 1. Generate Header Body
    h_content = """#pragma once
#include <string>
#include <cstring>
#include "cJSON.h"

namespace GeminiSkills {

// Raw Handshake Setup Payload compiled down to Flash memory
extern const char* const SETUP_HANDSHAKE_JSON;

enum class SkillType {
    UNKNOWN,
"""
    for skill in schema['skills'].keys():
        h_content += f"    {skill.upper()},\n"
    h_content += "};\n\n"

    # Generate specialized structure payloads
    for skill_name, body in schema['skills'].items():
        h_content += f"struct {skill_name}_args_t {{\n"
        if 'parameters' in body:
            for param, props in body['parameters'].items():
                if props['type'] == 'string':
                    h_content += f"    std::string {param};\n"
                elif props['type'] == 'int':
                    h_content += f"    int {param} = 0;\n"
                elif props['type'] == 'float':
                    h_content += f"    float {param} = 0.0f;\n"
                elif props['type'] == 'bool':
                    h_content += f"    bool {param} = false;\n"
        h_content += "};\n\n"

    h_content += """struct DecodedSkillCall {
    SkillType type = SkillType::UNKNOWN;
    char call_id[64] = {0};
    union {
"""
    for skill_name in schema['skills'].keys():
        h_content += f"        {skill_name}_args_t* {skill_name};\n"
    
    h_content += """    } args;

    DecodedSkillCall() { memset(&args, 0, sizeof(args)); }
    ~DecodedSkillCall();
};

// Safe PSRAM-bounded translation method
bool decode_incoming_arguments(const char* func_name, cJSON* args_obj, DecodedSkillCall& out_call);

} // namespace GeminiSkills
"""

    # 2. Generate C++ Source Layout
    setup_native = {
        "setup": {
            "model": "models/gemini-3.1-flash-live-preview",
            "generationConfig": { "responseModalities": ["AUDIO"] },
            # TODO: Re-enable tools once the core audio pipeline is stable and tested.
            # "tools": [{ "functionDeclarations": [] }]
        }
    }

    # TODO: Re-enable tool declaration generation once audio pipeline is stable.
    # for skill_name, body in schema['skills'].items():
    #     decl = {
    #         "name": skill_name,
    #         "description": body["description"],
    #         "parameters": {
    #             "type": "OBJECT",
    #             "properties": {},
    #             "required": body.get("required", [])
    #         }
    #     }
    #     if 'parameters' in body:
    #         # Gemini API type mapping: int→INTEGER, float→NUMBER, bool→BOOLEAN, string→STRING
    #         type_map = {'int': 'INTEGER', 'float': 'NUMBER', 'bool': 'BOOLEAN', 'string': 'STRING'}
    #         for param, props in body['parameters'].items():
    #             gemini_type = type_map.get(props['type'], props['type'].upper())
    #             decl["parameters"]["properties"][param] = {
    #                 "type": gemini_type
    #             }
    #             if "description" in props:
    #                 decl["parameters"]["properties"][param]["description"] = props["description"]
    #     setup_native["setup"]["tools"][0]["functionDeclarations"].append(decl)

    raw_json_escaped = json.dumps(setup_native).replace('"', '\\"')

    cpp_content = f"""#include "gemini_skills_generated.h"
#include "common/AppLogger.h"

namespace GeminiSkills {{

const char* const SETUP_HANDSHAKE_JSON = "{raw_json_escaped}";

DecodedSkillCall::~DecodedSkillCall() {{
    switch(type) {{
"""
    for skill_name in schema['skills'].keys():
        cpp_content += f"        case SkillType::{skill_name.upper()}: delete args.{skill_name}; break;\n"
    cpp_content += """        default: break;
    }
}

bool decode_incoming_arguments(const char* func_name, cJSON* args_obj, DecodedSkillCall& out_call) {
    if (!func_name || !args_obj) return false;
"""
    
    first = True
    for skill_name, body in schema['skills'].items():
        if first:
            cpp_content += f'    if (strcmp(func_name, "{skill_name}") == 0) {{\n'
            first = False
        else:
            cpp_content += f'    }} else if (strcmp(func_name, "{skill_name}") == 0) {{\n'
            
        cpp_content += f"        out_call.type = SkillType::{skill_name.upper()};\n"
        cpp_content += f"        out_call.args.{skill_name} = new {skill_name}_args_t();\n"
        
        if 'parameters' in body:
            for param, props in body['parameters'].items():
                cpp_content += f'        cJSON* item_{param} = cJSON_GetObjectItem(args_obj, "{param}");\n'
                cpp_content += f"        if (item_{param}) {{\n"
                if props['type'] == 'string':
                    cpp_content += f"            if (cJSON_IsString(item_{param})) out_call.args.{skill_name}->{param} = item_{param}->valuestring;\n"
                elif props['type'] == 'int':
                    cpp_content += f"            if (cJSON_IsNumber(item_{param})) out_call.args.{skill_name}->{param} = item_{param}->valueint;\n"
                elif props['type'] == 'float':
                    cpp_content += f"            if (cJSON_IsNumber(item_{param})) out_call.args.{skill_name}->{param} = item_{param}->valuedouble;\n"
                elif props['type'] == 'bool':
                    cpp_content += f"            if (cJSON_IsBool(item_{param})) out_call.args.{skill_name}->{param} = cJSON_IsTrue(item_{param});\n"
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
