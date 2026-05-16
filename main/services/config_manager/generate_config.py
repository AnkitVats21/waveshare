import json
import os
import sys

def generate_code(schema_path, output_h, output_cpp):
    with open(schema_path, 'r') as f:
        schema = json.load(f)

    h_content = """#pragma once
#include <string.h>
#include <stdbool.h>

namespace Services {

"""
    cpp_content = """#include "config_generated.h"
#include <string.h>
#include <stdlib.h>

namespace Services {

"""

    # Generate Structs
    for section, fields in schema.items():
        h_content += f"struct {section.capitalize()}Config {{\n"
        for field, props in fields.items():
            ftype = props['type']
            if ftype == 'string':
                size = props.get('size', 64)
                h_content += f"    char {field}[{size}];\n"
            elif ftype == 'bool':
                h_content += f"    bool {field};\n"
            elif ftype == 'int':
                h_content += f"    int {field};\n"
            elif ftype == 'float':
                h_content += f"    float {field};\n"
        h_content += "};\n\n"

    # Generate Main SystemConfig Struct
    h_content += "struct SystemConfig {\n"
    for section in schema.keys():
        h_content += f"    {section.capitalize()}Config {section};\n"
    h_content += "};\n\n"

    # Function declarations
    h_content += "void InitializeDefaultConfig(SystemConfig* config);\n"
    h_content += "bool UpdateConfigValue(SystemConfig* config, const char* section, const char* key, const char* value);\n"
    h_content += "\n} // namespace Services\n"

    # Implementation: InitializeDefaultConfig
    cpp_content += "void InitializeDefaultConfig(SystemConfig* config) {\n"
    cpp_content += "    memset(config, 0, sizeof(SystemConfig));\n"
    for section, fields in schema.items():
        for field, props in fields.items():
            default_val = props['default']
            ftype = props['type']
            if ftype == 'string':
                cpp_content += f'    strncpy(config->{section}.{field}, "{default_val}", sizeof(config->{section}.{field}) - 1);\n'
            elif ftype == 'bool':
                cpp_content += f"    config->{section}.{field} = {'true' if default_val else 'false'};\n"
            elif ftype == 'int':
                cpp_content += f"    config->{section}.{field} = {default_val};\n"
            elif ftype == 'float':
                cpp_content += f"    config->{section}.{field} = {default_val}f;\n"
    cpp_content += "}\n\n"

    # Implementation: UpdateConfigValue
    cpp_content += "bool UpdateConfigValue(SystemConfig* config, const char* section, const char* key, const char* value) {\n"
    first_section = True
    for section, fields in schema.items():
        if first_section:
            cpp_content += f'    if (strcmp(section, "{section}") == 0) {{\n'
            first_section = False
        else:
            cpp_content += f'    }} else if (strcmp(section, "{section}") == 0) {{\n'
        
        first_field = True
        for field, props in fields.items():
            ftype = props['type']
            if first_field:
                cpp_content += f'        if (strcmp(key, "{field}") == 0) {{\n'
                first_field = False
            else:
                cpp_content += f'        }} else if (strcmp(key, "{field}") == 0) {{\n'
            
            if ftype == 'string':
                cpp_content += f'            strncpy(config->{section}.{field}, value, sizeof(config->{section}.{field}) - 1);\n'
                cpp_content += f'            config->{section}.{field}[sizeof(config->{section}.{field}) - 1] = \'\\0\';\n'
            elif ftype == 'bool':
                cpp_content += f'            config->{section}.{field} = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);\n'
            elif ftype == 'int':
                cpp_content += f'            config->{section}.{field} = atoi(value);\n'
            elif ftype == 'float':
                cpp_content += f'            config->{section}.{field} = (float)atof(value);\n'
            cpp_content += "            return true;\n"
        cpp_content += "        }\n"
    cpp_content += "    }\n"
    cpp_content += "    return false;\n"
    cpp_content += "}\n\n"
    cpp_content += "} // namespace Services\n"

    with open(output_h, 'w') as f:
        f.write(h_content)
    with open(output_cpp, 'w') as f:
        f.write(cpp_content)

if __name__ == "__main__":
    if len(sys.argv) < 4:
        print("Usage: python generate_config.py <schema_json> <output_h> <output_cpp>")
        sys.exit(1)
    generate_code(sys.argv[1], sys.argv[2], sys.argv[3])
