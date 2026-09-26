"""
starc db: compiles nexus_db schema files (schema/db/*.star) into C++ document
structs and a JS schema for the dashboard reader (tools/starc/js/ndb.js).

    database music path="/sdcard/db/music.ndb" flush=batched {
        collection tracks id=1 key=string cache doc=Track {
            field title:       string        tag=1
            field duration_ms: u32           tag=4
            field volume:      u8 = 80       tag=5   // default after '='
        }
    }

Tags are never reused. Removing a field: delete the line and keep its tag
retired (a comment helps). See docs/nexus-db-design.md.
"""

import argparse
import json
import os
import re
import sys
from typing import List, Optional

TYPES = {
    # type: (C++ type, Writer method, byte size or None, default)
    "bool": ("bool", "boolean", 1, "false"),
    "u8": ("uint8_t", "u8", 1, "0"),
    "i8": ("int8_t", "i8", 1, "0"),
    "u16": ("uint16_t", "u16", 2, "0"),
    "i16": ("int16_t", "i16", 2, "0"),
    "u32": ("uint32_t", "u32", 4, "0"),
    "i32": ("int32_t", "i32", 4, "0"),
    "u64": ("uint64_t", "u64", 8, "0"),
    "i64": ("int64_t", "i64", 8, "0"),
    "f32": ("float", "f32", 4, "0"),
    "string": ("std::string", "str", None, '""'),
    "bytes": ("std::string", "bytes", None, '""'),
    "rgb": ("nexus_db::Rgb", "rgb", 3, "0x000000"),
}
FLUSH = {"every_commit": "EveryCommit", "batched": "Batched"}


class DbField:
    def __init__(self, name, type_, tag, default, line):
        self.name, self.type, self.tag, self.default, self.line = name, type_, tag, default, line


class DbCollection:
    def __init__(self, name, cid, key, cached, doc):
        self.name, self.id, self.key, self.cached, self.doc = name, cid, key, cached, doc
        self.fields: List[DbField] = []


class DbSchema:
    def __init__(self, name, path, flush, source):
        self.name, self.path, self.flush, self.source = name, path, flush, source
        self.collections: List[DbCollection] = []

    @property
    def class_name(self):
        return camel(self.name) + "Db"


def camel(name: str) -> str:
    return "".join(p[:1].upper() + p[1:] for p in name.split("_"))


class SchemaError(Exception):
    pass


DB_RE = re.compile(r'database\s+(\w+)\s+(.*?)\{$')
COLL_RE = re.compile(r'collection\s+(\w+)\s+(.*?)\{$')
FIELD_RE = re.compile(r'field\s+(\w+)\s*:\s*(\w+)\s*(?:=\s*(".*?"|[^\s]+))?\s+tag=(\d+)$')
ATTR_RE = re.compile(r'(\w+)(?:=("[^"]*"|\S+))?')


def attrs(text: str) -> dict:
    out = {}
    for m in ATTR_RE.finditer(text):
        v = m.group(2)
        out[m.group(1)] = v.strip('"') if v is not None else True
    return out


def parse(content: str, source: str) -> DbSchema:
    schema: Optional[DbSchema] = None
    coll: Optional[DbCollection] = None
    for lineno, raw in enumerate(content.splitlines(), 1):
        line = raw.split("//", 1)[0].strip()
        if not line:
            continue
        where = f"{source}:{lineno}"
        if line == "}":
            if coll is not None:
                coll = None
            elif schema is not None:
                pass
            continue
        m = DB_RE.match(line)
        if m:
            if schema is not None:
                raise SchemaError(f"{where}: one database per file")
            a = attrs(m.group(2))
            if "path" not in a:
                raise SchemaError(f"{where}: database needs path=")
            flush = a.get("flush", "every_commit")
            if flush not in FLUSH:
                raise SchemaError(f"{where}: flush must be one of {', '.join(FLUSH)}")
            if len(m.group(1)) > 11:
                raise SchemaError(f"{where}: database name is at most 11 characters")
            schema = DbSchema(m.group(1), a["path"], flush, source)
            continue
        m = COLL_RE.match(line)
        if m:
            if schema is None or coll is not None:
                raise SchemaError(f"{where}: collection must be directly inside a database")
            a = attrs(m.group(2))
            cid = int(a.get("id", 0))
            if not 1 <= cid <= 255:
                raise SchemaError(f"{where}: collection id must be 1..255")
            if any(c.id == cid for c in schema.collections):
                raise SchemaError(f"{where}: collection id {cid} used twice")
            if a.get("key", "string") != "string":
                raise SchemaError(f"{where}: only key=string is supported")
            coll = DbCollection(m.group(1), cid, "string", "cache" in a, a.get("doc", camel(m.group(1))))
            schema.collections.append(coll)
            continue
        m = FIELD_RE.match(line)
        if m:
            if coll is None:
                raise SchemaError(f"{where}: field outside a collection")
            name, type_, default, tag = m.group(1), m.group(2), m.group(3), int(m.group(4))
            if type_ not in TYPES:
                raise SchemaError(f"{where}: unknown type '{type_}' (one of {', '.join(TYPES)})")
            if not 1 <= tag <= 65535:
                raise SchemaError(f"{where}: tag must be 1..65535")
            if any(f.tag == tag for f in coll.fields):
                raise SchemaError(f"{where}: tag {tag} used twice in {coll.name}")
            if any(f.name == name for f in coll.fields):
                raise SchemaError(f"{where}: field {name} defined twice")
            coll.fields.append(DbField(name, type_, tag, default, lineno))
            continue
        raise SchemaError(f"{where}: cannot parse '{line}'")
    if schema is None:
        raise SchemaError(f"{source}: no database")
    for c in schema.collections:
        if len(c.fields) > 64:
            raise SchemaError(f"{source}: {c.name} has more than 64 fields")
    return schema


def schema_hash(s: DbSchema) -> int:
    # Covers what decides the layout; defaults and comments don't count.
    text = s.name + ";" + ";".join(
        f"{c.id}:{c.name}:{int(c.cached)}:" + ",".join(f"{f.tag}={f.type}" for f in sorted(c.fields, key=lambda f: f.tag))
        for c in sorted(s.collections, key=lambda c: c.id))
    h = 2166136261
    for b in text.encode():
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


# --- defaults ---------------------------------------------------------------

def default_value(f: DbField):
    """The default as a Python value (for JS output)."""
    d = f.default if f.default is not None else TYPES[f.type][3]
    if f.type in ("string", "bytes"):
        if not (d.startswith('"') and d.endswith('"')):
            raise SchemaError(f"{f.name}: string default must be quoted")
        return json.loads(d)
    if f.type == "bool":
        if d not in ("true", "false"):
            raise SchemaError(f"{f.name}: bool default must be true or false")
        return d == "true"
    if f.type == "f32":
        return float(d.rstrip("f"))
    return int(d, 0)


def cpp_default(f: DbField) -> str:
    v = default_value(f)
    if f.type in ("string", "bytes"):
        return "" if v == "" else json.dumps(v)
    if f.type == "bool":
        return "true" if v else "false"
    if f.type == "f32":
        return repr(float(v)) + "f"
    if f.type == "rgb":
        return f"{{0x{(v >> 16) & 0xFF:02X}, 0x{(v >> 8) & 0xFF:02X}, 0x{v & 0xFF:02X}}}"
    if f.type in ("u64",):
        return f"{v}ull"
    if f.type in ("i64",):
        return f"{v}ll"
    if f.type == "u32":
        return f"{v}u"
    return str(v)


# --- C++ --------------------------------------------------------------------

def gen_header(s: DbSchema) -> str:
    ns = s.name
    o = []
    o.append(f"// Generated by starc from {os.path.basename(s.source)}. Do not edit.")
    o.append("#pragma once")
    o.append("")
    o.append("#include <cstdint>")
    o.append("#include <string>")
    o.append("#include <string_view>")
    o.append("")
    o.append('#include "nexus_db/Collection.h"')
    o.append('#include "nexus_db/Database.h"')
    o.append('#include "nexus_db/Fields.h"')
    o.append("")
    o.append(f"namespace ndb::{ns} {{")
    o.append("")
    o.append(f"constexpr uint32_t SCHEMA_HASH = 0x{schema_hash(s):08X}u;")
    o.append("")
    for c in s.collections:
        o.append(f"// Collection '{c.name}' (id {c.id}, key: {c.key}{', cached' if c.cached else ''}).")
        o.append(f"struct {c.doc} {{")
        o.append(f"    static constexpr uint8_t COLLECTION = {c.id};")
        o.append("    enum : uint64_t {")
        for i, f in enumerate(c.fields):
            o.append(f"        F_{f.name.upper()} = 1ull << {i},")
        all_mask = "0" if not c.fields else f"0x{(1 << len(c.fields)) - 1:X}ull"
        o.append(f"        F_ALL = {all_mask},")
        o.append("    };")
        o.append("")
        for f in c.fields:
            d = cpp_default(f)
            init = f"{{}}" if d == "" else (f" = {d}" if not d.startswith("{") else d)
            o.append(f"    {TYPES[f.type][0]} {f.name}{init};")
        o.append("")
        o.append("    void encode(nexus_db::Writer& w, uint64_t fields = F_ALL) const;")
        o.append("    // Fields missing from `value` keep their current values.")
        o.append("    void decode(std::string_view value);")
        o.append("};")
        o.append("")
    cls = s.class_name
    o.append(f"// {s.path}")
    o.append(f"class {cls} {{")
    o.append("public:")
    o.append("    static const nexus_db::Options& options();")
    o.append("")
    o.append(f"    explicit {cls}(nexus_db::Io& io) : m_db(options(), io) {{}}")
    o.append("#ifdef ESP_PLATFORM")
    o.append(f"    {cls}() : {cls}(nexus_db::sdIo()) {{}}")
    o.append("#endif")
    o.append("")
    o.append("    bool open() { return m_db.open(); }")
    o.append("    void close() { m_db.close(); }")
    o.append("    nexus_db::Database& db() { return m_db; }")
    o.append("")
    for c in s.collections:
        o.append(f"    nexus_db::Collection<{c.doc}> {c.name}() {{ return nexus_db::Collection<{c.doc}>(m_db); }}")
    o.append("")
    o.append("private:")
    o.append("    nexus_db::Database m_db;")
    o.append("};")
    o.append("")
    o.append(f"}}  // namespace ndb::{ns}")
    o.append("")
    return "\n".join(o)


def gen_source(s: DbSchema, header_name: str) -> str:
    ns = s.name
    o = []
    o.append(f"// Generated by starc from {os.path.basename(s.source)}. Do not edit.")
    o.append(f'#include "{header_name}"')
    o.append("")
    o.append(f"namespace ndb::{ns} {{")
    o.append("")
    for c in s.collections:
        o.append(f"void {c.doc}::encode(nexus_db::Writer& w, uint64_t fields) const {{")
        if not c.fields:
            o.append("    (void)w;")
            o.append("    (void)fields;")
        for f in c.fields:
            o.append(f"    if (fields & F_{f.name.upper()}) w.{TYPES[f.type][1]}({f.tag}, {f.name});")
        o.append("}")
        o.append("")
        o.append(f"void {c.doc}::decode(std::string_view value) {{")
        o.append("    nexus_db::FieldReader r(reinterpret_cast<const uint8_t*>(value.data()), value.size());")
        o.append("    uint16_t tag, len;")
        o.append("    const uint8_t* v;")
        o.append("    while (r.next(tag, v, len)) {")
        if c.fields:
            o.append("        switch (tag) {")
            for f in c.fields:
                o.append(f"            case {f.tag}: nexus_db::FieldReader::read(v, len, {f.name}); break;")
            o.append("            default: break;  // unknown tag: a newer or retired field")
            o.append("        }")
        o.append("    }")
        o.append("}")
        o.append("")
    o.append("static const nexus_db::CollectionDef COLLECTIONS[] = {")
    for c in s.collections:
        o.append(f'    {{{c.id}, "{c.name}", {"true" if c.cached else "false"}}},')
    o.append("};")
    o.append("")
    o.append(f"const nexus_db::Options& {s.class_name}::options() {{")
    o.append("    static const nexus_db::Options opts = [] {")
    o.append("        nexus_db::Options o{};")
    o.append(f'        o.name = "{s.name}";')
    o.append(f'        o.path = "{s.path}";')
    o.append("        o.schema_hash = SCHEMA_HASH;")
    o.append(f"        o.flush = nexus_db::Flush::{FLUSH[s.flush]};")
    o.append("        o.collections = COLLECTIONS;")
    o.append("        o.collection_count = sizeof(COLLECTIONS) / sizeof(COLLECTIONS[0]);")
    o.append("        return o;")
    o.append("    }();")
    o.append("    return opts;")
    o.append("}")
    o.append("")
    o.append(f"}}  // namespace ndb::{ns}")
    o.append("")
    return "\n".join(o)


# --- JS ---------------------------------------------------------------------

def gen_js(schemas: List[DbSchema]) -> str:
    out = {}
    for s in schemas:
        colls = {}
        for c in s.collections:
            colls[str(c.id)] = {
                "name": c.name,
                "key": c.key,
                "cached": c.cached,
                "fields": {str(f.tag): {"name": f.name, "type": f.type, "default": default_value(f)} for f in c.fields},
            }
        out[s.name] = {"hash": schema_hash(s), "path": s.path, "collections": colls}
    srcs = ", ".join(os.path.basename(s.source) for s in schemas)
    return (f"// Generated by starc from {srcs}. Do not edit.\n"
            f"// Read with ndb.js: readNdb(buffer, NDB_SCHEMAS.<name>).\n"
            f"export const NDB_SCHEMAS = {json.dumps(out, indent=2)};\n")


def write_if_changed(path: str, text: str):
    try:
        with open(path) as f:
            if f.read() == text:
                return
    except FileNotFoundError:
        pass
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)
    print(f"[starc db] wrote {path}")


def main(argv):
    p = argparse.ArgumentParser(prog="starc.py db", description="Compile nexus_db schemas")
    p.add_argument("schemas", nargs="+", help="schema/db/<name>.star files")
    p.add_argument("--out-dir", help="directory for <Name>Db.generated.h/.cpp")
    p.add_argument("--js", help="output path for the dashboard's ndb_schema.js")
    args = p.parse_args(argv)
    try:
        parsed = []
        for path in args.schemas:
            with open(path) as f:
                parsed.append(parse(f.read(), path))
        names = [s.name for s in parsed]
        if len(set(names)) != len(names):
            raise SchemaError("database names must be unique")
        if args.out_dir:
            for s in parsed:
                h = f"{s.class_name}.generated.h"
                write_if_changed(os.path.join(args.out_dir, h), gen_header(s))
                write_if_changed(os.path.join(args.out_dir, f"{s.class_name}.generated.cpp"), gen_source(s, h))
        if args.js:
            write_if_changed(args.js, gen_js(parsed))
    except SchemaError as e:
        print(f"starc db: error: {e}", file=sys.stderr)
        return 1
    return 0
