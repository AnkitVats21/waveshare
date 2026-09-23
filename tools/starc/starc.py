#!/usr/bin/env python3
"""
STAR Schema Compiler (starc)
Compiles declarative .star schema files into C++ headers, codecs, and bindings.
"""

import sys
import re
import argparse
from typing import List, Dict, Optional, Tuple, Any

class EnumEntry:
    def __init__(self, name: str, value: Optional[str] = None):
        self.name = name
        self.value = value

class EnumDef:
    def __init__(self, name: str, underlying_type: Optional[str] = None):
        self.name = name
        self.underlying_type = underlying_type
        self.entries: List[EnumEntry] = []

class Field:
    def __init__(self, name: str, type_str: str, default_val: str, attrs: List[str]):
        self.name = name
        self.type_str = type_str
        self.default_val = default_val
        self.attrs = [a.strip() for a in attrs]
        
        # Determine access level
        if "writable" in self.attrs:
            self.access = "FieldAccess::Writable"
        elif "piorigin" in self.attrs:
            self.access = "FieldAccess::PiOrigin"
        else:
            self.access = "FieldAccess::ReadOnly"
            
        # Determine bit
        self.bit_index = None
        self.bit_name = None
        for a in self.attrs:
            if a.startswith("bit="):
                val = a[4:]
                if ":" in val:
                    idx_s, bname = val.split(":", 1)
                    self.bit_index = int(idx_s)
                    self.bit_name = bname.strip()
                else:
                    self.bit_index = int(val)
                    self.bit_name = self.name.upper()
                break
                
        # Check string size
        self.str_size = None
        m = re.match(r"^string\[(\d+)\]$", self.type_str)
        if m:
            self.str_size = int(m.group(1))
            self.is_str = True
        else:
            self.is_str = False
            
        self.is_color = "color" in self.attrs

class Component:
    def __init__(self, name: str, comp_id: int, mask: str):
        self.name = name
        self.comp_id = comp_id
        self.mask = mask
        self.fields: List[Field] = []
        self.aliases: List[Tuple[str, str]] = []

class Schema:
    def __init__(self):
        self.enums: List[EnumDef] = []
        self.top_aliases: List[Tuple[str, str]] = []
        self.components: List[Component] = []

def parse_star_schema(content: str) -> Schema:
    schema = Schema()
    current_comp: Optional[Component] = None
    current_enum: Optional[EnumDef] = None
    
    comp_pattern = re.compile(r"component\s+(\w+)\s+id=(\d+)\s+mask=(0x[0-9a-fA-F]+)\s*\{")
    enum_pattern = re.compile(r"enum\s+(\w+)(?:\s*:\s*(\w+))?\s*\{")
    alias_pattern = re.compile(r"alias\s+([^\s=]+)\s*=\s*([^\s]+)")
    field_pattern = re.compile(r"field\s+(\w+)\s*:\s*([^\s=]+)\s*=\s*(.*?)\s*\[(.*?)\]")
    
    for line in content.splitlines():
        line = line.strip()
        if not line or line.startswith("//"):
            continue
            
        if current_enum is not None:
            if line == "}":
                current_enum = None
                continue
            entry_line = line.rstrip(",")
            if "=" in entry_line:
                ename, evalue = entry_line.split("=", 1)
                current_enum.entries.append(EnumEntry(ename.strip(), evalue.strip()))
            else:
                current_enum.entries.append(EnumEntry(entry_line.strip()))
            continue
            
        m_enum = enum_pattern.match(line)
        if m_enum:
            current_enum = EnumDef(m_enum.group(1), m_enum.group(2))
            schema.enums.append(current_enum)
            continue
            
        m_comp = comp_pattern.match(line)
        if m_comp:
            current_comp = Component(
                name=m_comp.group(1),
                comp_id=int(m_comp.group(2)),
                mask=m_comp.group(3)
            )
            schema.components.append(current_comp)
            continue
            
        if line == "}":
            current_comp = None
            continue
            
        m_alias = alias_pattern.match(line)
        if m_alias:
            a_src = m_alias.group(1).strip()
            a_dst = m_alias.group(2).strip()
            if current_comp is not None:
                current_comp.aliases.append((a_src, a_dst))
            else:
                schema.top_aliases.append((a_src, a_dst))
            continue
            
        if current_comp is not None:
            m_field = field_pattern.match(line)
            if m_field:
                attrs = [a.strip() for a in m_field.group(4).split(",") if a.strip()]
                field = Field(
                    name=m_field.group(1).strip(),
                    type_str=m_field.group(2).strip(),
                    default_val=m_field.group(3).strip(),
                    attrs=attrs
                )
                current_comp.fields.append(field)
                
    return schema

def generate_header(schema: Schema) -> str:
    out = []
    out.append("// Generated by starc (STAR Schema Compiler). DO NOT EDIT MANUALLY.")
    out.append("#pragma once")
    out.append("")
    out.append('#include "core_sysdb/app_types.h"')
    out.append('#include "core_sysdb/led_types.h"')
    out.append('#include "core_sysdb/SysDbTypes.h"')
    out.append('#include "core_sysdb/AudioRates.h"')
    out.append("#include <cstdint>")
    out.append("#include <string>")
    out.append("#include <cstring>")
    out.append("")
    
    # 1. Component identifiers
    out.append("// ─────────────────────────────────────────────────────────────────────────────")
    out.append("// Component-level change bitmasks")
    out.append("// ─────────────────────────────────────────────────────────────────────────────")
    out.append("using ComponentMask = uint32_t;")
    out.append("")
    out.append("namespace COMP {")
    for comp in schema.components:
        out.append(f"    static constexpr ComponentMask {comp.name.upper():<12} = {comp.mask}u;")
    for src, dst in schema.top_aliases:
        s = src.replace("COMP::", "")
        d = dst.replace("COMP::", "")
        out.append(f"    static constexpr ComponentMask {s:<12} = {d};")
    out.append("    static constexpr ComponentMask ALL          = 0xFFFF0000u;")
    out.append("}")
    out.append("")
    
    # 2. Enums defined in schema
    if schema.enums:
        out.append("// ─────────────────────────────────────────────────────────────────────────────")
        out.append("// Enums defined in STAR schema")
        out.append("// ─────────────────────────────────────────────────────────────────────────────")
        for e in schema.enums:
            utype = f" : {e.underlying_type}" if e.underlying_type else ""
            out.append(f"enum class {e.name}{utype} {{")
            for idx, entry in enumerate(e.entries):
                sep = "," if idx < len(e.entries) - 1 else ""
                if entry.value is not None:
                    out.append(f"    {entry.name} = {entry.value}{sep}")
                else:
                    out.append(f"    {entry.name}{sep}")
            out.append("};")
            out.append("")
            
    # 3. Per-field notification bits
    out.append("// ─────────────────────────────────────────────────────────────────────────────")
    out.append("// Per-field notification bits")
    out.append("// ─────────────────────────────────────────────────────────────────────────────")
    for comp in schema.components:
        has_bits = any(f.bit_index is not None for f in comp.fields)
        if has_bits:
            out.append(f"namespace BIT_{comp.name.upper()} {{")
            defined_bits = set()
            for f in comp.fields:
                if f.bit_index is not None and f.bit_name not in defined_bits:
                    bit_val = f"(1u << {f.bit_index})"
                    out.append(f"    static constexpr ComponentMask {f.bit_name:<16} = {bit_val};")
                    defined_bits.add(f.bit_name)
            for src, dst in comp.aliases:
                if not src.startswith("BIT_"):
                    out.append(f"    static constexpr ComponentMask {src:<16} = {dst};")
            out.append("}")
            for src, dst in comp.aliases:
                if src.startswith("BIT_"):
                    out.append(f"namespace {src} = {dst};")
            out.append("")
            
    # 4. X-macro definitions
    out.append("// ─────────────────────────────────────────────────────────────────────────────")
    out.append("// SystemState Field Definitions with FieldAccess Annotations")
    out.append("// ─────────────────────────────────────────────────────────────────────────────")
    for comp in schema.components:
        out.append(f"#define {comp.name.upper()}_FIELDS \\")
        lines = []
        for f in comp.fields:
            if f.bit_index is not None:
                bit_ref = f"BIT_{comp.name.upper()}::{f.bit_name}"
            else:
                bit_ref = "0"
                
            if f.is_str:
                lines.append(f'    X_STR({f.name}, {f.str_size}, {f.default_val}, {bit_ref}, {f.access})')
            elif f.is_color:
                lines.append(f'    X_COLOR({f.name}, {f.default_val}, {bit_ref}, {f.access})')
            else:
                lines.append(f'    X({f.type_str}, {f.name}, {f.default_val}, {bit_ref}, {f.access})')
        out.append(" \\\n".join(lines))
        out.append("")
        
    # 5. Full SystemState struct
    out.append("// ─────────────────────────────────────────────────────────────────────────────")
    out.append("// SystemState — the single shared in-memory database")
    out.append("// ─────────────────────────────────────────────────────────────────────────────")
    out.append("struct SystemState {")
    out.append("    #define X(type, name, def, bit, access) type name = def;")
    out.append("    #define X_STR(name, size, def, bit, access) char name[size] = def;")
    out.append("    #define X_COLOR(name, def, bit, access) RgbColor name = def;")
    out.append("")
    for comp in schema.components:
        out.append(f"    // ── COMP::{comp.name.upper()} ───────────────────────────────────────────")
        out.append("    struct {")
        out.append(f"        {comp.name.upper()}_FIELDS")
        out.append(f"    }} {comp.name.lower()};")
        out.append("")
    out.append("    #undef X")
    out.append("    #undef X_STR")
    out.append("    #undef X_COLOR")
    out.append("};")
    out.append("")
    
    # 6. Sequential field tags
    out.append("// ─────────────────────────────────────────────────────────────────────────────")
    out.append("// STAR Wire Tag Namespaces (Sequential uint8_t field indices per component)")
    out.append("// ─────────────────────────────────────────────────────────────────────────────")
    for comp in schema.components:
        out.append(f"namespace TAG_{comp.name.upper()} {{")
        out.append("    enum : uint8_t {")
        out.append("        #define X(type, name, def, bit, access) name,")
        out.append("        #define X_STR(name, size, def, bit, access) name,")
        out.append("        #define X_COLOR(name, def, bit, access) name,")
        out.append(f"        {comp.name.upper()}_FIELDS")
        out.append("        #undef X")
        out.append("        #undef X_STR")
        out.append("        #undef X_COLOR")
        out.append("        _COUNT")
        out.append("    };")
        out.append("}")
        out.append("")
        
    # 7. Helper functions
    out.append("// ─────────────────────────────────────────────────────────────────────────────")
    out.append("// Component and Field Helpers")
    out.append("// ─────────────────────────────────────────────────────────────────────────────")
    out.append("inline constexpr ComponentMask getComponentMask(ComponentId comp) {")
    out.append("    return (1u << (16 + static_cast<uint8_t>(comp)));")
    out.append("}")
    out.append("")
    out.append("inline constexpr ComponentId getComponentId(ComponentMask comp) {")
    out.append("    switch (comp & 0xFFFF0000u) {")
    for comp in schema.components:
        out.append(f"        case COMP::{comp.name.upper()}: return ComponentId::{comp.name.upper()};")
    out.append("        default: return ComponentId::COUNT;")
    out.append("    }")
    out.append("}")
    out.append("")
    
    out.append("inline FieldAccess getFieldAccess(ComponentId comp, uint8_t field_tag) {")
    out.append("    switch (comp) {")
    for comp in schema.components:
        out.append(f"        case ComponentId::{comp.name.upper()}: {{")
        out.append("            static constexpr FieldAccess s_access[] = {")
        out.append("                #define X(type, name, def, bit, access) access,")
        out.append("                #define X_STR(name, size, def, bit, access) access,")
        out.append("                #define X_COLOR(name, def, bit, access) access,")
        out.append(f"                {comp.name.upper()}_FIELDS")
        out.append("                #undef X")
        out.append("                #undef X_STR")
        out.append("                #undef X_COLOR")
        out.append("            };")
        out.append("            if (field_tag < sizeof(s_access)/sizeof(s_access[0])) return s_access[field_tag];")
        out.append("            break;")
        out.append("        }")
    out.append("        default: break;")
    out.append("    }")
    out.append("    return FieldAccess::ReadOnly;")
    out.append("}")
    out.append("")
    
    out.append("inline uint8_t getFieldCount(ComponentId comp) {")
    out.append("    switch (comp) {")
    for comp in schema.components:
        out.append(f"        case ComponentId::{comp.name.upper()}: return TAG_{comp.name.upper()}::_COUNT;")
    out.append("        default: return 0;")
    out.append("    }")
    out.append("}")
    out.append("")
    
    out.append("inline const char* getFieldName(ComponentId comp, uint8_t field_tag) {")
    out.append("    switch (comp) {")
    for comp in schema.components:
        out.append(f"        case ComponentId::{comp.name.upper()}: {{")
        out.append("            static const char* const s_names[] = {")
        out.append("                #define X(type, name, def, bit, access) #name,")
        out.append("                #define X_STR(name, size, def, bit, access) #name,")
        out.append("                #define X_COLOR(name, def, bit, access) #name,")
        out.append(f"                {comp.name.upper()}_FIELDS")
        out.append("                #undef X")
        out.append("                #undef X_STR")
        out.append("                #undef X_COLOR")
        out.append("            };")
        out.append("            if (field_tag < sizeof(s_names)/sizeof(s_names[0])) return s_names[field_tag];")
        out.append("            break;")
        out.append("        }")
    out.append("        default: break;")
    out.append("    }")
    out.append('    return "unknown";')
    out.append("}")
    out.append("")
    
    out.append("inline bool isFieldWritable(ComponentId comp, uint8_t field_tag) {")
    out.append("    FieldAccess acc = getFieldAccess(comp, field_tag);")
    out.append("    return acc != FieldAccess::ReadOnly;")
    out.append("}")
    out.append("")
    
    return "\n".join(out)

def generate_codec(schema: Schema) -> str:
    out = []
    out.append("// Generated by starc (STAR Schema Compiler). DO NOT EDIT MANUALLY.")
    out.append('#include "core_sysdb/SysDbCodec.h"')
    out.append("#include <cstring>")
    out.append("")
    out.append("namespace SysDbCodec {")
    out.append("")
    
    # diffState: per-field change detection -> OR'd (COMP::X | BIT_X::FIELD) mask
    out.append("ComponentMask diffState(const SystemState& old_s, const SystemState& new_s) {")
    out.append("    ComponentMask changed = 0;")
    out.append("")
    for comp in schema.components:
        c, C = comp.name.lower(), comp.name.upper()
        out.append(f"    // ── {C}_FIELDS ──────────────────────────────────────────")
        out.append("    {")
        out.append("        #define MARK(bit) changed |= ((bit) != 0 ? (COMP::%s | (bit)) : COMP::%s)" % (C, C))
        out.append(f"        #define X(type, name, def, bit, access) if (old_s.{c}.name != new_s.{c}.name) MARK(bit);")
        out.append(f"        #define X_STR(name, size, def, bit, access) if (std::strncmp(old_s.{c}.name, new_s.{c}.name, size) != 0) MARK(bit);")
        out.append(f"        #define X_COLOR(name, def, bit, access) if (old_s.{c}.name.r != new_s.{c}.name.r || old_s.{c}.name.g != new_s.{c}.name.g || old_s.{c}.name.b != new_s.{c}.name.b) MARK(bit);")
        out.append(f"        {C}_FIELDS")
        out.append("        #undef X")
        out.append("        #undef X_STR")
        out.append("        #undef X_COLOR")
        out.append("        #undef MARK")
        out.append("    }")
        out.append("")
    out.append("    return changed;")
    out.append("}")
    out.append("")
    out.append("} // namespace SysDbCodec")
    out.append("")
    return "\n".join(out)

def main():
    parser = argparse.ArgumentParser(description="STAR Schema Compiler (starc)")
    parser.add_argument("schema", help="Path to input .star schema file")
    parser.add_argument("--out-header", help="Path to output C++ generated header")
    parser.add_argument("--out-codec", help="Path to output C++ generated codec")
    args = parser.parse_args()
    
    with open(args.schema, "r") as f:
        content = f.read()
        
    schema = parse_star_schema(content)
    print(f"[starc] Parsed {len(schema.enums)} enums, {len(schema.components)} components from {args.schema}")
    for c in schema.components:
        print(f"  - {c.name:<10} (id={c.comp_id}, mask={c.mask}, fields={len(c.fields)})")
        
    if args.out_header:
        header_src = generate_header(schema)
        with open(args.out_header, "w") as f:
            f.write(header_src)
        print(f"[starc] Generated header: {args.out_header}")
        
    if args.out_codec:
        codec_src = generate_codec(schema)
        with open(args.out_codec, "w") as f:
            f.write(codec_src)
        print(f"[starc] Generated codec: {args.out_codec}")

if __name__ == "__main__":
    main()
