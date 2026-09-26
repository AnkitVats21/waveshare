#!/usr/bin/env node
// Prints a nexus_db file as JSON: node ndb_dump.mjs <file.ndb> <ndb_schema.js>
import { readFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';
import { resolve } from 'node:path';
import { readNdb } from './ndb.js';

const [file, schemaPath] = process.argv.slice(2);
if (!file || !schemaPath) {
  console.error('usage: ndb_dump.mjs <file.ndb> <ndb_schema.js>');
  process.exit(2);
}
const { NDB_SCHEMAS } = await import(pathToFileURL(resolve(schemaPath)).href);
const bytes = new Uint8Array(readFileSync(file));
const name = new TextDecoder().decode(bytes.subarray(16, 28)).replace(/\0.*$/s, '');
const schema = NDB_SCHEMAS[name];
if (!schema) {
  console.error(`no schema for database '${name}'`);
  process.exit(1);
}
const db = readNdb(bytes, schema);
const collections = {};
for (const [coll, docs] of Object.entries(db.collections)) {
  collections[coll] = Object.fromEntries([...docs.entries()].sort(([a], [b]) => (a < b ? -1 : a > b ? 1 : 0)));
}
const replacer = (_, v) => (v instanceof Uint8Array ? Buffer.from(v).toString('hex') : v);
console.log(JSON.stringify({ header: db.header, schemaMatches: db.schemaMatches, stats: db.stats, collections }, replacer, 2));
