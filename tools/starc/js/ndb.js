// Reader for nexus_db files (docs/nexus-db-design.md), shared by the dashboard
// and the host tests. It applies the same rules as the device at open: check
// the header, read records in order, drop records that fail their CRC, latest
// record wins, merges replace fields, deletes remove.
//
//   const db = readNdb(arrayBuffer, NDB_SCHEMAS.music);
//   db.collections.tracks  // Map(key -> { title, artist, ... })

const HEADER_SIZE = 32;
const RECORD_HEADER = 16;
const SYNC_MARKER = 0x5aa5;
const MAX_KEY = 64;
const OP_PUT = 1, OP_MERGE = 2, OP_DELETE = 3, OP_SEAL = 4;

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[i] = c >>> 0;
  }
  return t;
})();

export function crc32(bytes, start = 0, end = bytes.length) {
  let c = 0xffffffff;
  for (let i = start; i < end; i++) c = CRC_TABLE[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

const utf8 = new TextDecoder('utf-8');

// Fields of a value as [tag, Uint8Array] pairs; null if one overruns.
function fieldsOf(bytes, start, end) {
  const out = [];
  let p = start;
  while (end - p >= 4) {
    const tag = bytes[p] | (bytes[p + 1] << 8);
    const len = bytes[p + 2] | (bytes[p + 3] << 8);
    if (end - p - 4 < len) return null;
    out.push([tag, bytes.subarray(p + 4, p + 4 + len)]);
    p += 4 + len;
  }
  return out;
}

const SIZES = { bool: 1, u8: 1, i8: 1, u16: 2, i16: 2, u32: 4, i32: 4, u64: 8, i64: 8, f32: 4, rgb: 3 };

function decodeValue(type, b) {
  const size = SIZES[type];
  if (size !== undefined && b.length !== size) return undefined;  // changed type: use the default
  const dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
  switch (type) {
    case 'bool': return b[0] !== 0;
    case 'u8': return b[0];
    case 'i8': return dv.getInt8(0);
    case 'u16': return dv.getUint16(0, true);
    case 'i16': return dv.getInt16(0, true);
    case 'u32': return dv.getUint32(0, true);
    case 'i32': return dv.getInt32(0, true);
    case 'u64': return Number(dv.getBigUint64(0, true));
    case 'i64': return Number(dv.getBigInt64(0, true));
    case 'f32': return dv.getFloat32(0, true);
    case 'rgb': return (b[0] << 16) | (b[1] << 8) | b[2];
    case 'string': return utf8.decode(b);
    case 'bytes': return b.slice();
    default: return undefined;
  }
}

function decodeDoc(fields, collection) {
  const doc = {};
  for (const f of Object.values(collection.fields)) doc[f.name] = f.default;
  for (const [tag, bytes] of fields) {
    const f = collection.fields[tag];
    if (!f) continue;  // unknown tag: a newer or retired field
    const v = decodeValue(f.type, bytes);
    if (v !== undefined) doc[f.name] = v;
  }
  return doc;
}

// Returns the record at `pos`, or null if it isn't a valid one.
function parseRecord(bytes, dv, pos) {
  if (pos + RECORD_HEADER > bytes.length || dv.getUint16(pos, true) !== SYNC_MARKER) return null;
  const len = dv.getUint16(pos + 2, true);
  if (len < RECORD_HEADER || (len & 3) || pos + len > bytes.length) return null;
  if (crc32(bytes, pos + 8, pos + len) !== dv.getUint32(pos + 4, true)) return null;
  const op = bytes[pos + 13];
  const keyLen = bytes[pos + 14];
  if (op < OP_PUT || op > OP_SEAL || keyLen > MAX_KEY || RECORD_HEADER + keyLen > len) return null;
  const fields = fieldsOf(bytes, pos + RECORD_HEADER + keyLen, pos + len);
  if (!fields) return null;
  return {
    len,
    seq: dv.getUint32(pos + 8, true),
    collection: bytes[pos + 12],
    op,
    key: utf8.decode(bytes.subarray(pos + RECORD_HEADER, pos + RECORD_HEADER + keyLen)),
    fields,
  };
}

export function readNdb(buffer, schema) {
  const bytes = buffer instanceof Uint8Array ? buffer : new Uint8Array(buffer);
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (bytes.length < HEADER_SIZE || utf8.decode(bytes.subarray(0, 4)) !== 'NXDB' ||
      crc32(bytes, 0, 28) !== dv.getUint32(28, true)) {
    throw new Error('not a nexus_db file');
  }
  const nameBytes = bytes.subarray(16, 28);
  const nul = nameBytes.indexOf(0);
  const header = {
    version: dv.getUint16(4, true),
    schemaHash: dv.getUint32(8, true),
    generation: dv.getUint32(12, true),
    name: utf8.decode(nul < 0 ? nameBytes : nameBytes.subarray(0, nul)),
  };
  if (header.version > 1) throw new Error(`format version ${header.version} is newer than this reader`);

  // Per collection: Map(key -> Map(tag -> bytes)), folded like the device does.
  const raw = new Map();
  const stats = { records: 0, skipped: 0, truncated: 0 };
  let pos = HEADER_SIZE;
  while (pos < bytes.length) {
    const rec = parseRecord(bytes, dv, pos);
    if (rec) {
      stats.records++;
      pos += rec.len;
      const coll = schema.collections[rec.collection];
      if (rec.op === OP_SEAL || !coll) continue;
      if (!raw.has(rec.collection)) raw.set(rec.collection, new Map());
      const docs = raw.get(rec.collection);
      if (rec.op === OP_DELETE) {
        docs.delete(rec.key);
      } else if (rec.op === OP_PUT) {
        docs.set(rec.key, new Map(rec.fields));
      } else if (rec.op === OP_MERGE && coll.cached) {
        const doc = docs.get(rec.key) ?? new Map();
        for (const [tag, b] of rec.fields) {
          doc.delete(tag);  // keep the order the device's merge produces
          doc.set(tag, b);
        }
        docs.set(rec.key, doc);
      }
      continue;
    }
    let next = pos + 4;
    while (next + RECORD_HEADER <= bytes.length && !parseRecord(bytes, dv, next)) next += 4;
    if (next + RECORD_HEADER > bytes.length) {
      stats.truncated = bytes.length - pos;  // a write in progress, or cut off
      break;
    }
    stats.skipped += next - pos;
    pos = next;
  }

  const collections = {};
  for (const [id, coll] of Object.entries(schema.collections)) {
    const out = new Map();
    for (const [key, fields] of raw.get(Number(id)) ?? []) out.set(key, decodeDoc(fields, coll));
    collections[coll.name] = out;
  }
  return { header, schemaMatches: header.schemaHash === schema.hash, collections, stats };
}
