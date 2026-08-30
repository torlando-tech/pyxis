// Copyright (c) 2026 Pyxis contributors
// SPDX-License-Identifier: MIT

import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import test from 'node:test';
import {
  decodeActiveSelection,
  encodeActiveMapSet,
  getMuiStyleProfile,
  inspectMuiZip,
  installMuiZip,
  parseSparseManifest,
  planActivation,
  resolveMuiStyleProfile,
  serializeIndexlessManifest,
  suggestMapIdentity,
  validateCandidatePacks,
} from '../../docs/flasher/js/map-installer.js';

const PNG = Buffer.from(
  'iVBORw0KGgoAAAANSUhEUgAAAQAAAAEACAIAAADTED8xAAAA1UlEQVR4nO3BMQEAAADCoPVP7WULoAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACAGwEtAAHMpTgHAAAAAElFTkSuQmCC',
  'base64',
);

test('each selected archive receives a fresh suggested name and pack ID', () => {
  assert.deepEqual(suggestMapIdentity('regional-overview.zip'), {
    name: 'regional-overview',
    packId: 'regional-overview',
  });
  assert.deepEqual(suggestMapIdentity('Local Detail Tiles.ZIP'), {
    name: 'Local Detail Tiles',
    packId: 'local-detail-tiles',
  });
  assert.deepEqual(suggestMapIdentity('.zip'), {
    name: 'Offline map',
    packId: 'offline-map',
  });
});

function crc32(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit++) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
  }
  return (~crc) >>> 0;
}

function pngWithChunkBeforeIdat(png, type, payload) {
  let position = 8;
  while (png.toString('ascii', position + 4, position + 8) !== 'IDAT') {
    position += 12 + png.readUInt32BE(position);
  }
  const name = Buffer.from(type, 'ascii');
  const data = Buffer.from(payload);
  const chunk = Buffer.alloc(12 + data.length);
  chunk.writeUInt32BE(data.length, 0); name.copy(chunk, 4); data.copy(chunk, 8);
  chunk.writeUInt32BE(crc32(Buffer.concat([name, data])), 8 + data.length);
  return Buffer.concat([png.subarray(0, position), chunk, png.subarray(position)]);
}

function rewriteFirstIdat(png, transform) {
  let position = 8;
  while (png.toString('ascii', position + 4, position + 8) !== 'IDAT') position += 12 + png.readUInt32BE(position);
  const oldLength = png.readUInt32BE(position);
  const payload = Buffer.from(transform(png.subarray(position + 8, position + 8 + oldLength)));
  const type = Buffer.from('IDAT');
  const chunk = Buffer.alloc(12 + payload.length);
  chunk.writeUInt32BE(payload.length, 0); type.copy(chunk, 4); payload.copy(chunk, 8);
  chunk.writeUInt32BE(crc32(Buffer.concat([type, payload])), 8 + payload.length);
  return Buffer.concat([png.subarray(0, position), chunk, png.subarray(position + 12 + oldLength)]);
}

function storedZip(entries) {
  const local = [];
  const central = [];
  let offset = 0;
  for (const [name, payload] of entries) {
    const filename = Buffer.from(name, 'utf8');
    const data = Buffer.from(payload);
    const checksum = crc32(data);
    const header = Buffer.alloc(30);
    header.writeUInt32LE(0x04034b50, 0);
    header.writeUInt16LE(20, 4);
    header.writeUInt16LE(0, 6);
    header.writeUInt16LE(0, 8);
    header.writeUInt32LE(checksum, 14);
    header.writeUInt32LE(data.length, 18);
    header.writeUInt32LE(data.length, 22);
    header.writeUInt16LE(filename.length, 26);
    local.push(header, filename, data);

    const record = Buffer.alloc(46);
    record.writeUInt32LE(0x02014b50, 0);
    record.writeUInt16LE(20, 4);
    record.writeUInt16LE(20, 6);
    record.writeUInt16LE(0, 8);
    record.writeUInt16LE(0, 10);
    record.writeUInt32LE(checksum, 16);
    record.writeUInt32LE(data.length, 20);
    record.writeUInt32LE(data.length, 24);
    record.writeUInt16LE(filename.length, 28);
    record.writeUInt32LE(offset, 42);
    central.push(record, filename);
    offset += header.length + filename.length + data.length;
  }
  const centralBytes = Buffer.concat(central);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0);
  end.writeUInt16LE(entries.length, 8);
  end.writeUInt16LE(entries.length, 10);
  end.writeUInt32LE(centralBytes.length, 12);
  end.writeUInt32LE(offset, 16);
  return new Blob([...local, centralBytes, end], {type: 'application/zip'});
}

class MemoryFileHandle {
  constructor(name, log) { this.name = name; this.kind = 'file'; this.bytes = new Uint8Array(); this.log = log; this.failOnWrite = false; }
  async createWritable() {
    const handle = this;
    let pending = new Uint8Array();
    return {
      async write(value) { pending = new Uint8Array(value instanceof Blob ? await value.arrayBuffer() : value); },
      async close() {
        if (handle.failOnWrite) throw new Error(`injected write failure: ${handle.name}`);
        handle.bytes = pending; handle.log.push(`write:${handle.name}`);
      },
      async abort() {},
    };
  }
  async getFile() { return new Blob([this.bytes]); }
}

class MemoryDirectoryHandle {
  constructor(name = '', log = []) { this.name = name; this.kind = 'directory'; this.log = log; this.children = new Map(); }
  async getDirectoryHandle(name, options = {}) {
    const current = this.children.get(name);
    if (current) {
      if (current.kind !== 'directory') throw new DOMException('type', 'TypeMismatchError');
      return current;
    }
    if (!options.create) throw new DOMException('missing', 'NotFoundError');
    const child = new MemoryDirectoryHandle(name, this.log);
    this.children.set(name, child);
    return child;
  }
  async getFileHandle(name, options = {}) {
    const current = this.children.get(name);
    if (current) {
      if (current.kind !== 'file') throw new DOMException('type', 'TypeMismatchError');
      return current;
    }
    if (!options.create) throw new DOMException('missing', 'NotFoundError');
    const child = new MemoryFileHandle(name, this.log);
    this.children.set(name, child);
    return child;
  }
  async *entries() { for (const entry of this.children.entries()) yield entry; }
  async isSameEntry(other) { return this === other; }
  async removeEntry(name, options = {}) {
    const current = this.children.get(name);
    if (!current) throw new DOMException('missing', 'NotFoundError');
    if (current.kind === 'directory' && current.children.size && !options.recursive) throw new DOMException('not empty', 'InvalidModificationError');
    this.children.delete(name);
  }
}

class DirectoryHandleAlias {
  constructor(target) { this.target = target; this.name = target.name; this.kind = 'directory'; }
  async getDirectoryHandle(...args) { return this.target.getDirectoryHandle(...args); }
  async getFileHandle(...args) { return this.target.getFileHandle(...args); }
  async *entries() { yield* this.target.entries(); }
  async removeEntry(...args) { return this.target.removeEntry(...args); }
  async isSameEntry(other) { return this.target === (other?.target || other); }
}

class MemoryLockManager {
  constructor() { this.tails = new Map(); }
  async request(name, _options, operation) {
    const previous = this.tails.get(name) || Promise.resolve();
    let release;
    const gate = new Promise(resolve => { release = resolve; });
    const tail = previous.then(() => gate);
    this.tails.set(name, tail);
    await previous;
    try { return await operation(); }
    finally {
      release();
      if (this.tails.get(name) === tail) this.tails.delete(name);
    }
  }
}

async function child(root, path) {
  let current = root;
  for (const part of path.split('/')) current = current.children.get(part);
  return current;
}

async function fileAt(root, path) {
  const parts = path.split('/');
  let current = root;
  for (const part of parts.slice(0, -1)) current = current.children.get(part);
  const file = current.children.get(parts.at(-1));
  if (!file || file.kind !== 'file') throw new Error(`test helper: not a file: ${path}`);
  return new Uint8Array(await (await file.getFile()).arrayBuffer());
}

test('filesystem mock matches browser File System Access API semantics', async () => {
  // Repeated create:true on an existing file must return the same handle,
  // exactly like browsers (no exclusive-create behavior).
  const root = new MemoryDirectoryHandle();
  const first = await root.getFileHandle('a.pmas', {create: true});
  const second = await root.getFileHandle('a.pmas', {create: true});
  assert.equal(first, second);

  // The File System Access API has no `exclusive` option; unknown options
  // are ignored rather than converted into exceptions.
  const ignored = await root.getFileHandle('a.pmas', {create: true, exclusive: true});
  assert.equal(ignored, first);
  const directoryFirst = await root.getDirectoryHandle('tiles', {create: true});
  const directorySecond = await root.getDirectoryHandle('tiles', {create: true, exclusive: true});
  assert.equal(directoryFirst, directorySecond);

  // A file and directory sharing a name are TypeMismatchError in both
  // directions; missing files without create are NotFoundError.
  await assert.rejects(
    root.getFileHandle('tiles', {create: true}),
    error => error.name === 'TypeMismatchError',
  );
  const reverse = new MemoryDirectoryHandle();
  await reverse.getDirectoryHandle('a.pmas', {create: true});
  await assert.rejects(
    reverse.getFileHandle('a.pmas', {create: true}),
    error => error.name === 'TypeMismatchError',
  );
  await assert.rejects(
    root.getFileHandle('missing.pmas'),
    error => error.name === 'NotFoundError',
  );
  await assert.rejects(
    root.getDirectoryHandle('missing-dir', {create: false}),
    error => error.name === 'NotFoundError',
  );
});

const v3Vectors = JSON.parse(
  (await readFile(new URL('../fixtures/map_pack_v3_vectors.json', import.meta.url))).toString('utf8'),
);

function hexToBytes(hex) {
  return Uint8Array.from(hex.match(/../g).map(part => Number.parseInt(part, 16)));
}

function rewriteCrc32(bytes) {
  const output = new Uint8Array(bytes.length);
  output.set(bytes);
  const view = new DataView(output.buffer);
  view.setUint32(output.length - 4, crc32(output.subarray(0, output.length - 4)), true);
  return output;
}

test('browser consumes the frozen PMPK v3 vectors', () => {
  for (const name of ['pmpk_v3_one_tile', 'pmpk_v3_multi_zoom']) {
    const entry = v3Vectors[name];
    const parsed = parseSparseManifest(hexToBytes(entry.hex));
    assert.equal(parsed.packId, entry.pack_id);
    assert.equal(parsed.name, entry.name);
    assert.equal(parsed.attribution, entry.attribution);
    assert.equal(parsed.source, entry.source);
    assert.equal(parsed.license, entry.license);
    assert.equal(parsed.minZoom, entry.min_zoom);
    assert.equal(parsed.maxZoom, entry.max_zoom);
    assert.equal(parsed.tileCount, entry.tile_count);
    assert.deepEqual(parsed.rowSpans, []);
  }
});

test('browser serializes the frozen PMPK v3 vectors exactly', () => {
  for (const name of ['pmpk_v3_one_tile', 'pmpk_v3_multi_zoom']) {
    const entry = v3Vectors[name];
    const actual = serializeIndexlessManifest({
      packId: entry.pack_id, name: entry.name, attribution: entry.attribution,
      source: entry.source, license: entry.license,
    }, entry.min_zoom, entry.max_zoom, entry.tile_count);
    assert.deepEqual([...actual], [...hexToBytes(entry.hex)]);
  }
});

test('browser decodes the frozen PMAS v3 vectors', () => {
  for (const name of ['pmas_v3_one_pack', 'pmas_v3_three_packs']) {
    const entry = v3Vectors[name];
    const decoded = decodeActiveSelection(hexToBytes(entry.hex));
    assert.equal(decoded.version, 3);
    assert.equal(decoded.generation, entry.generation);
    assert.equal(decoded.mapSetId, entry.map_set_id);
    assert.equal(decoded.attribution, entry.attribution);
    assert.deepEqual(
      decoded.packs.map(pack => pack.packId),
      entry.pack_ids,
    );
    for (const pack of decoded.packs) assert.deepEqual(pack.rowSpans, undefined);
  }
});

test('browser encodes the frozen PMAS v3 vectors exactly', () => {
  for (const name of ['pmas_v3_one_pack', 'pmas_v3_three_packs']) {
    const entry = v3Vectors[name];
    const actual = encodeActiveMapSet({
      generation: entry.generation,
      mapSetId: entry.map_set_id,
      attribution: entry.attribution,
      packs: entry.pack_ids.map(packId => ({packId})),
    });
    assert.deepEqual([...actual], [...hexToBytes(entry.hex)]);
  }
});

test('browser rejects the same invalid PMPK v3 mutations as the CLI', () => {
  const mutate = (apply) => {
    const data = hexToBytes(v3Vectors.pmpk_v3_one_tile.hex);
    apply(data);
    return rewriteCrc32(data);
  };
  assert.throws(() => parseSparseManifest(mutate(data => { data[5] = 1; })));
  // Splice four zero bytes before the CRC: the total-length field then
  // disagrees with the actual record size.
  const withTrailing = rewriteCrc32(hexToBytes(v3Vectors.pmpk_v3_one_tile.hex));
  assert.throws(() => parseSparseManifest(
    new Uint8Array([
      ...withTrailing.subarray(0, withTrailing.length - 4),
      0, 0, 0, 0,
      ...withTrailing.subarray(withTrailing.length - 4),
    ]),
  ));
  const corrupted = hexToBytes(v3Vectors.pmpk_v3_one_tile.hex);
  corrupted[corrupted.length - 1] ^= 1;
  assert.throws(() => parseSparseManifest(corrupted));
  assert.throws(() => parseSparseManifest(mutate(data => {
    new DataView(data.buffer).setUint32(data.length - 8, 0, true);
  })));
  assert.throws(() => parseSparseManifest(mutate(data => { data[data.length - 9] = 1; })));
});

test('browser rejects the same invalid PMAS v3 mutations as the CLI', () => {
  const hex = v3Vectors.pmas_v3_one_pack.hex;
  const corrupted = hexToBytes(hex);
  corrupted[corrupted.length - 1] ^= 1;
  assert.throws(() => decodeActiveSelection(corrupted));
  const generationZero = hexToBytes(hex);
  new DataView(generationZero.buffer).setUint32(8, 0, true);
  assert.throws(() => decodeActiveSelection(rewriteCrc32(generationZero)));
  const trailing = rewriteCrc32(hexToBytes(hex));
  const extended = new Uint8Array(trailing.length + 1);
  extended.set(trailing);
  extended[trailing.length - 1] = trailing[trailing.length - 1];
  assert.throws(() => decodeActiveSelection(extended));
  assert.throws(() => encodeActiveMapSet({
    generation: 0, mapSetId: 'osm-bright', attribution: 'Example', packs: [{packId: 'detail'}],
  }));
  assert.throws(() => encodeActiveMapSet({
    generation: 1, mapSetId: 'osm-bright', attribution: 'Example',
    packs: Array.from({length: 9}, (_, index) => ({packId: `pack-${index}`})),
  }));
  assert.throws(() => encodeActiveMapSet({
    generation: 1, mapSetId: 'osm-bright', attribution: 'Example', packs: [],
  }));
  assert.throws(() => encodeActiveMapSet({
    generation: 1, mapSetId: 'osm-bright', attribution: 'Example',
    packs: [{packId: 'detail'}, {packId: 'detail'}],
  }));
});

test('browser preserves legacy PMAS v1 decoding and v2 span validation', () => {
  const body = new Uint8Array(44);
  const view = new DataView(body.buffer);
  view.setUint32(0, 0x53414d50, true);
  body[4] = 1;
  view.setUint16(6, 48, true);
  view.setUint32(8, 5, true);
  body[12] = 8;
  body.set(new TextEncoder().encode('legacy-1'), 13);
  const record = new Uint8Array(body.length + 4);
  record.set(body);
  new DataView(record.buffer).setUint32(44, crc32(body), true);
  const decoded = decodeActiveSelection(record);
  assert.equal(decoded.version, 1);
  assert.equal(decoded.generation, 5);
  assert.equal(decoded.packId, 'legacy-1');

  // Hand-built v2: header, map-set id, attribution, one pack carrying one
  // row span; total length and CRC patched after the body is assembled.
  const parts = [];
  const push = (bytes) => parts.push(...bytes);
  const pushU16 = (value) => { parts.push(value & 0xff, value >> 8); };
  const pushU32 = (value) => {
    parts.push(value & 0xff, (value >> 8) & 0xff, (value >> 16) & 0xff, (value >>> 24) & 0xff);
  };
  const pushSized = (text) => { push([...new TextEncoder().encode(text)]); };
  pushU32(0x53414d50); parts.push(2, 0); pushU16(0); pushU32(9);
  parts.push(8); pushSized('legacy-1');
  parts.push(7); pushSized('Example');
  parts.push(1);
  parts.push(8); pushSized('legacy-1');
  pushU16(1);
  parts.push(1); pushU32(1); pushU32(0); pushU32(1);
  const inner = new Uint8Array(parts);
  const totalLength = inner.length + 4;
  inner[6] = totalLength & 0xff;
  inner[7] = totalLength >> 8;
  const v2Record = new Uint8Array(inner.length + 4);
  v2Record.set(inner);
  new DataView(v2Record.buffer).setUint32(v2Record.length - 4, crc32(inner), true);
  const v2Decoded = decodeActiveSelection(v2Record);
  assert.equal(v2Decoded.version, 2);
  assert.equal(v2Decoded.generation, 9);
  assert.equal(v2Decoded.mapSetId, 'legacy-1');
  assert.deepEqual(v2Decoded.packs, [
    {packId: 'legacy-1', rowSpans: [{zoom: 1, y: 1, xMinimum: 0, xMaximum: 1}]},
  ]);
});

const metadata = {
  packId: 'overview',
  mapSetId: 'osm-bright',
  name: 'Overview',
  attribution: '(c) OpenMapTiles (c) OpenStreetMap contributors',
  source: "Oxed's Map Tile Downloader (OSM Bright)",
  license: 'OSM ODbL; style CC-BY-4.0/BSD-3-Clause',
};

test('maps supported Coalition styles to separate map sets and provenance', async () => {
  const expected = [
    ['osm-bright', 'OSM Bright', '(c) OpenMapTiles (c) OpenStreetMap contributors',
      'OSM ODbL; style CC-BY-4.0/BSD-3-Clause'],
    ['dark-matter', 'Dark Matter', '(c) OpenMapTiles (c) OpenStreetMap contributors; style (c) CARTO',
      'OSM ODbL; style CC-BY-4.0/BSD-3-Clause (CARTO CC-BY-3.0)'],
    ['positron', 'Positron', '(c) OpenMapTiles (c) OpenStreetMap contributors; style (c) CARTO',
      'OSM ODbL; style CC-BY-4.0/BSD-3-Clause (CARTO CC-BY-3.0)'],
    ['toner', 'Toner', '(c) MapTiler (c) OpenStreetMap contributors',
      'OSM ODbL; style CC-BY-4.0/BSD-3-Clause (Stamen ISC)'],
  ];
  for (const [styleId, label, attribution, license] of expected) {
    assert.deepEqual(getMuiStyleProfile(styleId), {
      id: styleId,
      label,
      attribution,
      source: `Oxed's Map Tile Downloader (${label})`,
      license,
    });
    const report = await inspectMuiZip(storedZip([[`maps/${styleId}/2/1/1.png`, PNG]]));
    assert.equal(report.styleId, styleId);
  }
  assert.throws(() => getMuiStyleProfile('unknown-style'), /unsupported MUI map style/i);
});

test('rootless archive requires explicit style before SD mutation and then installs', async () => {
  const archive = storedZip([['2/1/1.png', PNG]]);
  const report = await inspectMuiZip(archive);
  assert.equal(report.styleId, null);
  const root = new MemoryDirectoryHandle();
  assert.throws(() => resolveMuiStyleProfile(report.styleId, ''), /select map style/i);
  assert.equal(root.children.has('pyxis-map'), false);

  const style = resolveMuiStyleProfile(report.styleId, 'positron');
  assert.equal(style.id, 'positron');
  const result = await installMuiZip({archive, rootDirectory:root, metadata:{
    ...metadata, mapSetId:style.id, attribution:style.attribution,
    source:style.source, license:style.license,
  }});
  assert.equal(result.tileCount, 1);
  assert.ok(root.children.has('pyxis-map'));
});

test('rejects installing a detected style into a different map set', async () => {
  const archive = storedZip([['maps/dark-matter/2/1/1.png', PNG]]);
  const root = new MemoryDirectoryHandle();
  await assert.rejects(
    installMuiZip({archive, rootDirectory:root, metadata}),
    /style.*selected map set/i,
  );
  assert.equal(root.children.has('pyxis-map'), false);
});

test('style-qualified MUI install publishes an indexless PMPK v3 manifest', async () => {
  const archive = storedZip([
    ['maps/osm-bright/2/1/1.png', PNG],
    ['maps/osm-bright/2/1/0.png', PNG],
  ]);
  const root = new MemoryDirectoryHandle();
  const result = await installMuiZip({archive, rootDirectory:root, metadata});
  assert.equal(result.resumed, false);
  const pack = await child(root, 'pyxis-map/packs/overview');
  const stored = await fileAt(root, 'pyxis-map/packs/overview/manifest.pmp');
  const parsed = parseSparseManifest(stored);
  assert.equal(parsed.packId, 'overview');
  assert.equal(parsed.name, 'Overview');
  assert.equal(parsed.attribution, metadata.attribution);
  assert.equal(parsed.source, metadata.source);
  assert.equal(parsed.license, metadata.license);
  assert.equal(parsed.minZoom, 2);
  assert.equal(parsed.maxZoom, 2);
  assert.equal(parsed.tileCount, 2);
  assert.deepEqual(parsed.rowSpans, []);
  // No ownership receipt or other stray entries may remain in the pack.
  assert.deepEqual([...pack.children.keys()].sort(), ['manifest.pmp', 'tiles']);
});

test('rootless ZIP with a selected style publishes the matching PMPK v3 manifest', async () => {
  const archive = storedZip([
    ['0/0/0.png', PNG],
    ['1/0/0.png', PNG],
    ['1/1/0.png', PNG],
    ['1/0/1.png', PNG],
    ['1/1/1.png', PNG],
  ]);
  const root = new MemoryDirectoryHandle();
  const style = resolveMuiStyleProfile(null, 'toner');
  const result = await installMuiZip({archive, rootDirectory:root, metadata:{
    ...metadata, mapSetId:style.id, attribution:style.attribution,
    source:style.source, license:style.license,
  }});
  assert.equal(result.resumed, false);
  const stored = await fileAt(root, 'pyxis-map/packs/overview/manifest.pmp');
  const parsed = parseSparseManifest(stored);
  assert.equal(parsed.attribution, style.attribution);
  assert.equal(parsed.source, style.source);
  assert.equal(parsed.license, style.license);
  assert.equal(parsed.minZoom, 0);
  assert.equal(parsed.maxZoom, 1);
  assert.equal(parsed.tileCount, 5);
  assert.deepEqual(parsed.rowSpans, []);
});

test('the PMPK v3 manifest is published after the last tile and read back', async () => {
  const archive = storedZip([['2/1/1.png', PNG], ['2/1/0.png', PNG]]);
  const root = new MemoryDirectoryHandle();
  const writes = [];
  const originalCreateWritable = MemoryFileHandle.prototype.createWritable;
  MemoryFileHandle.prototype.createWritable = async function () {
    const writable = await originalCreateWritable.call(this);
    const name = this.name;
    const originalClose = writable.close;
    writable.close = async () => { await originalClose.call(writable); writes.push(name); };
    return writable;
  };
  try {
    await installMuiZip({archive, rootDirectory:root, metadata});
  } finally {
    MemoryFileHandle.prototype.createWritable = originalCreateWritable;
  }
  const manifestIndex = writes.indexOf('manifest.pmp');
  assert.notEqual(manifestIndex, -1, 'manifest must be written');
  const tileNames = writes.filter(name => name.endsWith('.png'));
  assert.equal(tileNames.length, 2);
  for (const name of tileNames) {
    assert.ok(
      writes.indexOf(name) < manifestIndex,
      `tile ${name} must be written before the manifest: ${JSON.stringify(writes)}`,
    );
  }
  // The published manifest must decode from the on-disk bytes (read-back).
  const stored = await fileAt(root, 'pyxis-map/packs/overview/manifest.pmp');
  assert.equal(parseSparseManifest(stored).tileCount, 2);
});

test('policy mismatch after a detected style refuses before any SD write', async () => {
  const archive = storedZip([['maps/osm-bright/2/1/1.png', PNG]]);
  const root = new MemoryDirectoryHandle();
  await assert.rejects(
    installMuiZip({archive, rootDirectory:root,
      metadata:{...metadata, license:'CC-BY-3.0'}}),
    /attribution|provenance/i,
  );
  assert.equal(root.children.has('pyxis-map'), false);
});

test('rejects altered required style attribution before touching the SD root', async () => {
  const root = new MemoryDirectoryHandle();
  await assert.rejects(
    installMuiZip({archive:storedZip([['2/1/1.png', PNG]]),rootDirectory:root,
      metadata:{...metadata,attribution:'Map data'}}),
    /attribution|provenance/i,
  );
  assert.equal(root.children.has('pyxis-map'), false);
});

test('browser activation planning mirrors the CLI plan contract', () => {
  const attribution = getMuiStyleProfile('osm-bright').attribution;
  const slot = (generation, packIds, styleId = 'osm-bright') => encodeActiveMapSet({
    generation,
    mapSetId: styleId,
    attribution: getMuiStyleProfile(styleId).attribution,
    packs: packIds.map(packId => ({packId})),
  });
  const checkPlan = (plan, styleName, targetSlot, generation, packIds) => {
    assert.equal(plan.styleName, styleName);
    assert.equal(plan.targetSlot, targetSlot);
    assert.equal(plan.generation, generation);
    assert.deepEqual(plan.packIds, packIds);
    const decoded = decodeActiveSelection(plan.record);
    assert.equal(decoded.version, 3);
    assert.equal(decoded.generation, generation);
    assert.equal(decoded.mapSetId, styleName);
    assert.equal(decoded.attribution, getMuiStyleProfile(styleName).attribution);
    assert.deepEqual(decoded.packs.map(pack => pack.packId), packIds);
  };

  // Empty card: generation one, slot zero.
  checkPlan(planActivation({slot0: null, slot1: null, newPackId: 'pack-a',
    styleId: 'osm-bright', attribution}), 'osm-bright', 'active-pack.0', 1, ['pack-a']);

  // One valid slot: target the missing slot.
  checkPlan(planActivation({slot0: slot(1, ['pack-a']), slot1: null, newPackId: 'pack-b',
    styleId: 'osm-bright', attribution}), 'osm-bright', 'active-pack.1', 2, ['pack-b', 'pack-a']);

  // Two valid slots: overwrite the lower-generation one, inherit its composition.
  checkPlan(planActivation({slot0: slot(5, ['old-a']), slot1: slot(3, ['old-b']), newPackId: 'pack-c',
    styleId: 'osm-bright', attribution}), 'osm-bright', 'active-pack.1', 6, ['pack-c', 'old-a']);

  // Re-installing an existing pack moves it to the front without duplication.
  checkPlan(planActivation({slot0: slot(2, ['pack-a', 'pack-b']), slot1: null, newPackId: 'pack-b',
    styleId: 'osm-bright', attribution}), 'osm-bright', 'active-pack.1', 3, ['pack-b', 'pack-a']);

  // A different style starts a fresh composition but still advances the
  // generation past the existing slots.
  checkPlan(planActivation({slot0: slot(2, ['pack-a'], 'toner'), slot1: null, newPackId: 'pack-b',
    styleId: 'dark-matter', attribution: getMuiStyleProfile('dark-matter').attribution}),
    'dark-matter', 'active-pack.1', 3, ['pack-b']);

  // The style PMAS composition wins over the active slots and drives the
  // generation.
  checkPlan(planActivation({slot0: slot(2, ['slot-only']), slot1: null,
    styleRecord: slot(9, ['style-a', 'slot-only']), newPackId: 'new-pack',
    styleId: 'osm-bright', attribution}), 'osm-bright', 'active-pack.1', 10,
    ['new-pack', 'style-a', 'slot-only']);

  // Invalid raw slot bytes are treated like a missing slot: the target is
  // slot zero and the generation restarts from one, exactly like the CLI.
  const corrupted = slot(1, ['pack-a']);
  corrupted[corrupted.length - 1] ^= 1;
  checkPlan(planActivation({slot0: corrupted, slot1: null, newPackId: 'pack-b',
    styleId: 'osm-bright', attribution}), 'osm-bright', 'active-pack.0', 1, ['pack-b']);

  // Equal-generation disagreement is rejected before any plan exists.
  assert.throws(() => planActivation({slot0: slot(4, ['pack-a']), slot1: slot(4, ['pack-b']),
    newPackId: 'pack-c', styleId: 'osm-bright', attribution}), /equal generation/i);

  // The 8-pack limit is enforced before publication; re-installing an
  // existing pack within the limit is allowed.
  assert.throws(() => planActivation({
    slot0: slot(1, Array.from({length: 8}, (_, index) => `pack-${index}`)), slot1: null,
    newPackId: 'pack-new', styleId: 'osm-bright', attribution,
  }), /8-pack/i);
  checkPlan(planActivation({
    slot0: slot(1, Array.from({length: 8}, (_, index) => `pack-${index}`)), slot1: null,
    newPackId: 'pack-3', styleId: 'osm-bright', attribution,
  }), 'osm-bright', 'active-pack.1', 2,
    ['pack-3', ...Array.from({length: 8}, (_, index) => `pack-${index}`).filter(id => id !== 'pack-3')]);

  // Generation exhaustion is rejected.
  const MAX_GENERATION = 0xffffffff;
  assert.throws(() => planActivation({slot0: slot(MAX_GENERATION, ['pack-a']), slot1: null,
    newPackId: 'pack-b', styleId: 'osm-bright', attribution}), /exhausted/i);

  // Attribution must match the style policy exactly.
  assert.throws(() => planActivation({slot0: null, slot1: null, newPackId: 'pack-a',
    styleId: 'osm-bright', attribution: 'wrong'}), /attribution/i);
  assert.throws(() => planActivation({slot0: null, slot1: null, newPackId: 'pack-a',
    styleId: 'nope', attribution: 'x'}), /unsupported/i);
  assert.throws(() => planActivation({slot0: null, slot1: null, newPackId: 'Bad ID',
    styleId: 'osm-bright', attribution}), /pack id/i);

  // A style PMAS for a different map set or with the wrong attribution is
  // ignored, not trusted.
  checkPlan(planActivation({slot0: slot(2, ['slot-only']), slot1: null,
    styleRecord: slot(9, ['style-a'], 'toner'), newPackId: 'new-pack',
    styleId: 'osm-bright', attribution}), 'osm-bright', 'active-pack.1', 3,
    ['new-pack', 'slot-only']);
});

// Install a one-tile MUI pack, returning the root for follow-up mutation.
async function installPack(root, archive, meta) {
  return installMuiZip({archive, rootDirectory: root, metadata: meta});
}

async function packTreeBytes(root) {
  const pyxis = root.children.get('pyxis-map');
  const out = new Map();
  const walk = async (dir, prefix) => {
    for (const [name, child] of dir.children.entries()) {
      const path = prefix ? `${prefix}/${name}` : name;
      if (child.kind === 'file') {
        out.set(path, Buffer.from(await (await child.getFile()).arrayBuffer()));
      } else {
        await walk(child, path);
      }
    }
  };
  if (pyxis) await walk(pyxis, 'pyxis-map');
  return out;
}

test('browser validates every inherited pack before activation', async () => {
  const style = getMuiStyleProfile('osm-bright');
  const metaFor = (packId) => ({
    ...metadata, packId, attribution: style.attribution, source: style.source, license: style.license,
  });
  // Activate pack-a, then pack-b + pack-a, mirroring the CLI A8 fixture.
  const root = new MemoryDirectoryHandle();
  await installPack(root, storedZip([['maps/osm-bright/1/0/0.png', PNG]]), metaFor('pack-a'));
  await installPack(root, storedZip([['maps/osm-bright/1/1/0.png', PNG]]), metaFor('pack-b'));
  const pyxis = root.children.get('pyxis-map');
  const mapSets = pyxis.children.get('map-sets');
  const packs = pyxis.children.get('packs');
  const styleRecord = new Uint8Array(await (await mapSets.children.get('osm-bright.pmas').getFile()).arrayBuffer());
  const slot0 = new Uint8Array(await (await pyxis.children.get('active-pack.0').getFile()).arrayBuffer());
  const slot1 = pyxis.children.get('active-pack.1');
  const slot1Bytes = slot1
    ? new Uint8Array(await (await slot1.getFile()).arrayBuffer())
    : null;
  const before = await packTreeBytes(root);

  // Missing inherited pack: refusal before any record mutation.
  await packs.children.get('pack-a').removeEntry('manifest.pmp');
  await assert.rejects(
    installPack(root, storedZip([['maps/osm-bright/2/0/0.png', PNG]]), metaFor('pack-c')),
    /missing inherited pack manifest/i,
  );
  // The card is byte-identical after the refusal: both slots and the style
  // record unchanged, and pack-c was never published. (The only diff from
  // the pre-refusal tree is the intentionally removed pack-a manifest.)
  let after = await packTreeBytes(root);
  const afterRefusal = new Map(after);
  const beforeRefusal = new Map(before);
  afterRefusal.delete('pyxis-map/packs/pack-a/manifest.pmp');
  beforeRefusal.delete('pyxis-map/packs/pack-a/manifest.pmp');
  assert.deepEqual([...afterRefusal.entries()].sort(), [...beforeRefusal.entries()].sort());
  assert.equal(after.get('pyxis-map/map-sets/osm-bright.pmas').toString('hex'), Buffer.from(styleRecord).toString('hex'));
  assert.equal(after.get('pyxis-map/active-pack.0').toString('hex'), Buffer.from(slot0).toString('hex'));
  if (slot1Bytes) {
    assert.equal(after.get('pyxis-map/active-pack.1').toString('hex'), Buffer.from(slot1Bytes).toString('hex'));
  }
  assert.equal(packs.children.has('pack-c'), false);

  // A second refusal (now for a corrupt inherited manifest path: the
  // missing manifest) leaves the slots and style record untouched as well.
  await assert.rejects(
    installPack(root, storedZip([['maps/osm-bright/2/1/1.png', PNG]]), metaFor('pack-c')),
    /missing inherited pack manifest/i,
  );
  after = await packTreeBytes(root);
  assert.equal(after.get('pyxis-map/active-pack.0').toString('hex'), Buffer.from(slot0).toString('hex'));
  assert.equal(after.get('pyxis-map/map-sets/osm-bright.pmas').toString('hex'), Buffer.from(styleRecord).toString('hex'));
  assert.equal(packs.children.has('pack-c'), false);
});

test('browser corrupt-inherited-pack refusal keeps the card untouched', async () => {
  const style = getMuiStyleProfile('osm-bright');
  const metaFor = (packId) => ({
    ...metadata, packId, attribution: style.attribution, source: style.source, license: style.license,
  });
  const root = new MemoryDirectoryHandle();
  await installPack(root, storedZip([['maps/osm-bright/1/0/0.png', PNG]]), metaFor('pack-a'));
  const pyxis = root.children.get('pyxis-map');
  const packs = pyxis.children.get('packs');
  const before = await packTreeBytes(root);

  // Corrupt the inherited manifest, then install pack-c.
  const manifest = await fileAt(root, 'pyxis-map/packs/pack-a/manifest.pmp');
  const corrupt = new Uint8Array(manifest);
  corrupt[0] ^= 1;
  const corruptHandle = await packs.children.get('pack-a').getFileHandle('manifest.pmp', {create: true});
  const corruptWritable = await corruptHandle.createWritable({keepExistingData: false});
  await corruptWritable.write(corrupt);
  await corruptWritable.close();

  await assert.rejects(
    installPack(root, storedZip([['maps/osm-bright/2/0/0.png', PNG]]), metaFor('pack-c')),
    /manifest header or CRC is invalid|missing inherited/i,
  );

  const after = await packTreeBytes(root);
  // Restore the corrupted manifest's original bytes in the snapshot: every
  // other file must be byte-identical to the pre-corruption state.
  const afterExpected = new Map(after);
  afterExpected.set('pyxis-map/packs/pack-a/manifest.pmp', Buffer.from(manifest));
  assert.deepEqual([...afterExpected.entries()].sort(), [...before.entries()].sort());
  assert.equal(packs.children.has('pack-c'), false);
});

test('browser validateCandidatePacks rejects v1/v2 and policy-mismatched manifests', async () => {
  const style = getMuiStyleProfile('osm-bright');
  const root = new MemoryDirectoryHandle();
  await installPack(root, storedZip([['maps/osm-bright/1/0/0.png', PNG]]), {
    ...metadata, attribution: style.attribution, source: style.source, license: style.license,
  });
  const pyxis = root.children.get('pyxis-map');
  const packs = pyxis.children.get('packs');
  const policyCheck = () => validateCandidatePacks(
    pyxis, 'osm-bright', style.attribution, ['overview']);
  await policyCheck();

  // A legal legacy v2 manifest (spans present) must be rejected as not v3.
  const parts = [];
  const pushU16 = (value) => { parts.push(value & 0xff, value >> 8); };
  const pushU32 = (value) => {
    parts.push(value & 0xff, (value >> 8) & 0xff, (value >> 16) & 0xff, (value >>> 24) & 0xff);
  };
  const pushSized = (text) => {
    const bytes = [...new TextEncoder().encode(text)];
    parts.push(bytes.length, ...bytes);
  };
  pushU32(0x4b504d50); parts.push(2, 0); pushU16(16); pushU32(0); pushU32(0);
  pushSized('overview'); pushSized('Overview');
  pushSized(style.attribution); pushSized(style.source); pushSized(style.license);
  parts.push(1, 1); pushU16(1); pushU32(1);
  parts.push(1); pushU32(0); pushU32(0); pushU32(1);
  const inner = new Uint8Array(parts);
  const totalLength = inner.length + 4;
  inner[8] = totalLength & 0xff;
  inner[9] = (totalLength >> 8) & 0xff;
  inner[10] = (totalLength >> 16) & 0xff;
  inner[11] = (totalLength >>> 24) & 0xff;
  const v2Record = new Uint8Array(inner.length + 4);
  v2Record.set(inner);
  new DataView(v2Record.buffer).setUint32(v2Record.length - 4, crc32(inner), true);
  const replaceManifest = async (bytes) => {
    const handle = await packs.children.get('overview').getFileHandle('manifest.pmp', {create: true});
    const writable = await handle.createWritable({keepExistingData: false});
    await writable.write(bytes);
    await writable.close();
  };
  await replaceManifest(v2Record);
  await assert.rejects(policyCheck(), /not PMPK v3/i);

  // A v3 manifest declaring a different pack ID is rejected. Rebuild a legal
  // v3 manifest with a 6-byte ID so every later string offset stays exact.
  const renamedManifest = serializeIndexlessManifest({
    packId: 'otherm', name: 'Overview', attribution: style.attribution,
    source: style.source, license: style.license,
  }, 1, 1, 1);
  await replaceManifest(renamedManifest);
  await assert.rejects(policyCheck(), /different pack ID/i);
});

test('indexless active map-set wire format contains ordered pack IDs only', () => {
  const record = encodeActiveMapSet({generation:1,mapSetId:'osm-bright',attribution:'Map data attribution',packs:[
    {packId:'detail'},
    {packId:'overview'},
  ]});
  const decoded = decodeActiveSelection(record);
  assert.equal(decoded.version, 3);
  assert.deepEqual(decoded.packs.map(pack => pack.packId), ['detail','overview']);
  assert(record.length < 256);
  assert.equal(Buffer.from(record).toString('hex'),
    '504d415303004100010000000a6f736d2d627269676874144d61702064617461206174747269627574696f6e020664657461696c086f766572766965772bbcaa3f');
});

test('accepts a sanitized downloader-shaped rootless stored ZIP fixture', async () => {
  const bytes = await readFile(new URL('./fixtures/downloader-rootless-stored-descriptor.zip', import.meta.url));
  const archive = new Blob([bytes], {type: 'application/zip'});

  // Walk the ZIP structures instead of searching for signatures, which may also
  // occur in PNG payloads. This fixture intentionally exercises signed streaming
  // descriptors, a stricter shape than inspectMuiZip itself requires.
  const requireRange = (offset, length, label) => {
    assert(Number.isSafeInteger(offset) && offset >= 0 && length >= 0 && offset + length <= bytes.length,
      `${label} is outside the fixture`);
  };
  const u16 = (offset, label) => { requireRange(offset, 2, label); return bytes.readUInt16LE(offset); };
  const u32 = (offset, label) => { requireRange(offset, 4, label); return bytes.readUInt32LE(offset); };
  const eocdOffset = bytes.length - 22;
  requireRange(eocdOffset, 22, 'EOCD');
  assert.equal(u32(eocdOffset, 'EOCD signature'), 0x06054b50);
  assert.equal(u16(eocdOffset + 4, 'EOCD disk number'), 0);
  assert.equal(u16(eocdOffset + 6, 'EOCD central-directory disk'), 0);
  assert.equal(u16(eocdOffset + 8, 'EOCD disk record count'), 3);
  assert.equal(u16(eocdOffset + 10, 'EOCD record count'), 3);
  assert.equal(u16(eocdOffset + 20, 'EOCD comment length'), 0);
  const centralSize = u32(eocdOffset + 12, 'central-directory size');
  const centralOffset = u32(eocdOffset + 16, 'central-directory offset');
  requireRange(centralOffset, centralSize, 'central directory');
  assert.equal(centralOffset + centralSize, eocdOffset);

  const expectedEntries = [
    {name: '0/0/0.png', size: 270},
    {name: '1/0/0.png', size: 270},
    {name: '2/1/1.png', size: 270},
  ];
  const records = [];
  let position = centralOffset;
  for (const expected of expectedEntries) {
    requireRange(position, 46, `central header for ${expected.name}`);
    assert.equal(u32(position, `central signature for ${expected.name}`), 0x02014b50);
    const nameLength = u16(position + 28, `central name length for ${expected.name}`);
    const extraLength = u16(position + 30, `central extra length for ${expected.name}`);
    const commentLength = u16(position + 32, `central comment length for ${expected.name}`);
    const recordLength = 46 + nameLength + extraLength + commentLength;
    requireRange(position, recordLength, `central record for ${expected.name}`);
    const name = bytes.toString('utf8', position + 46, position + 46 + nameLength);
    assert.equal(name, expected.name);
    assert.equal(u16(position + 4, `creator for ${name}`), 20); // DOS creator, ZIP 2.0
    assert.equal(u16(position + 8, `flags for ${name}`), 0x0008);
    assert.equal(u16(position + 10, `method for ${name}`), 0);
    assert.equal(extraLength, 0);
    assert.equal(commentLength, 0);
    assert.equal(u16(position + 34, `disk start for ${name}`), 0);
    assert.equal(u32(position + 38, `external attributes for ${name}`), 0);
    const crc = u32(position + 16, `CRC for ${name}`);
    const compressedSize = u32(position + 20, `compressed size for ${name}`);
    const uncompressedSize = u32(position + 24, `uncompressed size for ${name}`);
    assert.equal(compressedSize, expected.size);
    assert.equal(uncompressedSize, expected.size);
    records.push({name, crc, compressedSize, uncompressedSize, localOffset: u32(position + 42, `local offset for ${name}`)});
    position += recordLength;
  }
  assert.equal(position, centralOffset + centralSize, 'central directory must contain exactly three records');

  for (let index = 0; index < records.length; index++) {
    const record = records[index];
    const local = record.localOffset;
    requireRange(local, 30, `local header for ${record.name}`);
    assert.equal(u32(local, `local signature for ${record.name}`), 0x04034b50);
    assert.equal(u16(local + 6, `local flags for ${record.name}`), 0x0008);
    assert.equal(u16(local + 8, `local method for ${record.name}`), 0);
    assert.equal(u32(local + 14, `local CRC for ${record.name}`), 0);
    assert.equal(u32(local + 18, `local compressed size for ${record.name}`), 0);
    assert.equal(u32(local + 22, `local uncompressed size for ${record.name}`), 0);
    const localNameLength = u16(local + 26, `local name length for ${record.name}`);
    const localExtraLength = u16(local + 28, `local extra length for ${record.name}`);
    assert.equal(localExtraLength, 0);
    requireRange(local + 30, localNameLength, `local name for ${record.name}`);
    assert.equal(bytes.toString('utf8', local + 30, local + 30 + localNameLength), record.name);

    const descriptor = local + 30 + localNameLength + localExtraLength + record.compressedSize;
    requireRange(descriptor, 16, `data descriptor for ${record.name}`);
    assert.equal(u32(descriptor, `descriptor signature for ${record.name}`), 0x08074b50);
    assert.equal(u32(descriptor + 4, `descriptor CRC for ${record.name}`), record.crc);
    assert.equal(u32(descriptor + 8, `descriptor compressed size for ${record.name}`), record.compressedSize);
    assert.equal(u32(descriptor + 12, `descriptor uncompressed size for ${record.name}`), record.uncompressedSize);
    assert.equal(descriptor + 16, records[index + 1]?.localOffset ?? centralOffset,
      `data descriptor for ${record.name} must immediately follow its payload`);
  }

  const report = await inspectMuiZip(archive);
  assert.equal(report.tileCount, 3);
  assert.equal(report.minZoom, 0);
  assert.equal(report.maxZoom, 2);
  assert.equal('rowSpans' in report, false);

  const root = new MemoryDirectoryHandle();
  const result = await installMuiZip({archive, rootDirectory: root, metadata});
  assert.equal(result.tileCount, 3);
  assert.deepEqual(
    [...(await child(root, 'pyxis-map/packs/overview/tiles/0/0/0.png')).bytes],
    [...PNG],
  );
});

test('inspects and transactionally installs a sparse stored MUI XYZ ZIP', async () => {
  const archive = storedZip([
    ['2/1/1.png', PNG],
    ['2/1/2.png', PNG],
    ['2/2/1.png', PNG],
  ]);
  const report = await inspectMuiZip(archive);
  assert.equal(report.tileCount, 3);
  assert.equal(report.minZoom, 2);
  assert.equal(report.maxZoom, 2);
  assert.equal(report.styleId, null);
  assert.equal('rowSpans' in report, false);

  const root = new MemoryDirectoryHandle();
  const result = await installMuiZip({archive, rootDirectory: root, metadata});
  assert.equal(result.tileCount, 3);
  const pack = await child(root, 'pyxis-map/packs/overview');
  const manifest = pack.children.get('manifest.pmp').bytes;
  assert.equal(Buffer.from(manifest.subarray(0, 4)).toString('ascii'), 'PMPK');
  assert.equal(manifest[4], 3);
  const parsed = parseSparseManifest(manifest);
  assert.equal(parsed.tileCount, 3);
  assert.equal(parsed.rowSpans.length, 0);
  const selection = decodeActiveSelection((await child(root, 'pyxis-map/active-pack.0')).bytes);
  assert.equal(selection.version, 3);
  assert.equal(selection.mapSetId, 'osm-bright');
  assert.deepEqual(selection.packs.map(pack => pack.packId), ['overview']);
  assert.equal(selection.generation, 1);
  const firstCatalog = decodeActiveSelection(
    (await child(root, 'pyxis-map/map-sets/osm-bright.pmas')).bytes,
  );
  assert.equal(firstCatalog.mapSetId, 'osm-bright');
  assert.deepEqual(firstCatalog.packs.map(pack => pack.packId), ['overview']);
  const firstManifestWrite = root.log.indexOf('write:manifest.pmp');
  assert(firstManifestWrite > root.log.lastIndexOf('write:1.png'));
  assert(root.log.indexOf('write:active-pack.0') > firstManifestWrite);

  const second = await installMuiZip({
    archive, rootDirectory:root,
    metadata:{...metadata, packId:'overview-two', name:'Overview Two'},
  });
  assert.equal(second.selectionFile, 'active-pack.1');
  const secondSelection = decodeActiveSelection((await child(root, 'pyxis-map/active-pack.1')).bytes);
  assert.equal(secondSelection.generation, 2);
  assert.deepEqual(secondSelection.packs.map(pack => pack.packId), ['overview-two','overview']);
  const secondCatalog = decodeActiveSelection(
    (await child(root, 'pyxis-map/map-sets/osm-bright.pmas')).bytes,
  );
  assert.deepEqual(secondCatalog.packs.map(pack => pack.packId), ['overview-two','overview']);
  const third = await installMuiZip({
    archive, rootDirectory:root,
    metadata:{...metadata, packId:'overview-three', name:'Overview Three'},
  });
  assert.equal(third.selectionFile, 'active-pack.0');
  const thirdSelection = decodeActiveSelection((await child(root, 'pyxis-map/active-pack.0')).bytes);
  assert.equal(thirdSelection.generation, 3);
  assert.deepEqual(thirdSelection.packs.map(pack => pack.packId), ['overview-three','overview-two','overview']);

  const resumed = await installMuiZip({
    archive, rootDirectory:root,
    metadata:{...metadata, packId:'overview-two', name:'Overview Two'},
  });
  assert.equal(resumed.resumed, true);
  const resumedSelection = decodeActiveSelection((await child(root, 'pyxis-map/active-pack.1')).bytes);
  assert.equal(resumedSelection.generation, 4);
  assert.deepEqual(resumedSelection.packs.map(pack => pack.packId), ['overview-two','overview-three','overview']);
});

test('resume rejects extra files that indexless firmware could display', async () => {
  const archive = storedZip([['2/1/1.png', PNG]]);
  const root = new MemoryDirectoryHandle();
  await installMuiZip({archive, rootDirectory:root, metadata});
  const tiles = await child(root, 'pyxis-map/packs/overview/tiles');
  const zoom = await tiles.getDirectoryHandle('9', {create:true});
  const x = await zoom.getDirectoryHandle('1', {create:true});
  const extra = await x.getFileHandle('1.png', {create:true});
  extra.bytes = new Uint8Array(PNG);

  await assert.rejects(
    installMuiZip({archive, rootDirectory:root, metadata}),
    /unexpected existing pack entry/i,
  );
});

test('resume repeats PNG signature admission against the selected ZIP', async () => {
  const validArchive = storedZip([['2/1/1.png', PNG]]);
  const root = new MemoryDirectoryHandle();
  await installMuiZip({archive:validArchive, rootDirectory:root, metadata});

  const malformed = Buffer.from(PNG);
  malformed[0] ^= 0xff;
  const installed = await child(root, 'pyxis-map/packs/overview/tiles/2/1/1.png');
  installed.bytes = new Uint8Array(malformed);
  const forgedArchive = storedZip([['2/1/1.png', malformed]]);

  await assert.rejects(
    installMuiZip({archive:forgedArchive, rootDirectory:root, metadata}),
    /invalid PNG signature/i,
  );
});

test('preserves separate installed-style PMAS snapshots when activation changes', async () => {
  const root = new MemoryDirectoryHandle();
  const bright = getMuiStyleProfile('osm-bright');
  const dark = getMuiStyleProfile('dark-matter');
  await installMuiZip({
    archive: storedZip([['maps/osm-bright/2/1/1.png', PNG]]),
    rootDirectory: root,
    metadata: {...metadata, packId:'bright-pack', mapSetId:bright.id,
      attribution:bright.attribution, source:bright.source, license:bright.license},
  });
  await installMuiZip({
    archive: storedZip([['maps/dark-matter/2/1/1.png', PNG]]),
    rootDirectory: root,
    metadata: {...metadata, packId:'dark-pack', mapSetId:dark.id,
      attribution:dark.attribution, source:dark.source, license:dark.license},
  });
  const brightCatalog = decodeActiveSelection(
    (await child(root, 'pyxis-map/map-sets/osm-bright.pmas')).bytes,
  );
  const darkCatalog = decodeActiveSelection(
    (await child(root, 'pyxis-map/map-sets/dark-matter.pmas')).bytes,
  );
  assert.equal(brightCatalog.mapSetId, 'osm-bright');
  assert.deepEqual(brightCatalog.packs.map(pack => pack.packId), ['bright-pack']);
  assert.equal(darkCatalog.mapSetId, 'dark-matter');
  assert.deepEqual(darkCatalog.packs.map(pack => pack.packId), ['dark-pack']);
  const active = decodeActiveSelection((await child(root, 'pyxis-map/active-pack.1')).bytes);
  assert.equal(active.mapSetId, 'dark-matter');
});

test('rejects traversal, duplicate keys, unsupported compression, and malformed PNGs', async () => {
  await assert.rejects(inspectMuiZip(storedZip([['../2/1/1.png', PNG]])), /path|traversal/i);
  await assert.rejects(inspectMuiZip(storedZip([['2/1/1.png', PNG], ['2/1/1.png', PNG]])), /duplicate/i);
  const malformed = Buffer.from(PNG); malformed[0] ^= 1;
  await assert.rejects(installMuiZip({
    archive:storedZip([['2/1/1.png', malformed]]),
    rootDirectory:new MemoryDirectoryHandle(), metadata,
  }), /PNG/i);
});

test('install rejects signature-valid PNGs the firmware cannot decode', async () => {
  const wrongSize = Buffer.from(PNG);
  wrongSize.writeUInt32BE(128, 16);
  wrongSize.writeUInt32BE(crc32(wrongSize.subarray(12, 29)), 29);
  await assert.rejects(installMuiZip({
    archive:storedZip([['2/1/1.png', wrongSize]]),
    rootDirectory:new MemoryDirectoryHandle(), metadata,
  }), /256x256/i);
});

test('removes only its receipt-owned contents after an interrupted write', async () => {
  const archive = storedZip([
    ['2/1/1.png', PNG],
    ['2/1/2.png', PNG],
  ]);
  const root = new MemoryDirectoryHandle();
  await assert.rejects(installMuiZip({
    archive,
    rootDirectory:root,
    metadata:{...metadata, packId:'interrupted', name:'Interrupted'},
    onProgress(progress) { if (progress.phase === 'write') throw new Error('cancelled'); },
  }), /cancelled/);
  const packs = root.children.get('pyxis-map').children.get('packs');
  const interrupted = packs.children.get('interrupted');
  assert.ok(interrupted);
  assert.deepEqual([...interrupted.children.keys()], []);
  const retried = await installMuiZip({
    archive,
    rootDirectory:root,
    metadata:{...metadata, packId:'interrupted', name:'Interrupted'},
  });
  assert.equal(retried.resumed, false);
  assert.ok(interrupted.children.has('manifest.pmp'));
});

test('cleanup preserves an unrelated file that appears in the destination', async () => {
  const archive = storedZip([['2/1/1.png', PNG]]);
  const root = new MemoryDirectoryHandle();
  await assert.rejects(installMuiZip({
    archive,
    rootDirectory:root,
    metadata:{...metadata, packId:'raced', name:'Raced'},
    onProgress(progress) {
      if (progress.phase !== 'write') return;
      const pack = root.children.get('pyxis-map').children.get('packs').children.get('raced');
      const foreign = new MemoryFileHandle('foreign.txt', root.log);
      foreign.bytes = new Uint8Array([9]); pack.children.set('foreign.txt', foreign);
      throw new Error('interrupted');
    },
  }), /could not be safely removed/i);
  const pack = await child(root, 'pyxis-map/packs/raced');
  assert.deepEqual([...pack.children.get('foreign.txt').bytes], [9]);
});

test('receipt cleanup failure preserves a fully published pack', async () => {
  const archive = storedZip([['2/1/1.png', PNG]]);
  const root = new MemoryDirectoryHandle();
  let injected = false;
  await assert.rejects(installMuiZip({
    archive, rootDirectory:root, metadata:{...metadata,packId:'published',name:'Published'},
    onProgress(progress) {
      if (injected || progress.phase !== 'write') return;
      injected = true;
      const pack = root.children.get('pyxis-map').children.get('packs').children.get('published');
      const remove = pack.removeEntry.bind(pack);
      pack.removeEntry = async (name, options) => {
        if (name.startsWith('.pyxis-install-owner-')) { pack.removeEntry = remove; throw new Error('injected receipt failure'); }
        return remove(name, options);
      };
    },
  }), /installed and verified.*receipt/i);
  const pack = await child(root, 'pyxis-map/packs/published');
  assert.ok(pack.children.has('manifest.pmp'));
  assert.deepEqual([...pack.children.get('tiles').children.get('2').children.get('1').children.get('1.png').bytes], [...PNG]);
});

test('concurrent installs on one selected root are serialized', async () => {
  const archive = storedZip([['2/1/1.png', PNG]]);
  const root = new MemoryDirectoryHandle();
  await Promise.all([
    installMuiZip({archive, rootDirectory:root, metadata:{...metadata, packId:'first-pack', name:'First Pack'}}),
    installMuiZip({archive, rootDirectory:root, metadata:{...metadata, packId:'second-pack', name:'Second Pack'}}),
  ]);
  const pyxis = root.children.get('pyxis-map');
  const slots = ['active-pack.0', 'active-pack.1']
    .map(name => pyxis.children.get(name))
    .filter(Boolean)
    .map(handle => decodeActiveSelection(handle.bytes));
  const newest = slots.sort((left, right) => right.generation - left.generation)[0];
  assert.equal(newest.generation, 2);
  assert.deepEqual(new Set(newest.packs.map(pack => pack.packId)), new Set(['first-pack', 'second-pack']));
});

test('distinct handles for one selected root are serialized across tabs', async () => {
  const archive = storedZip([['2/1/1.png', PNG]]);
  const root = new MemoryDirectoryHandle('sd-card');
  const firstHandle = new DirectoryHandleAlias(root);
  const secondHandle = new DirectoryHandleAlias(root);
  const previousNavigator = Object.getOwnPropertyDescriptor(globalThis, 'navigator');
  Object.defineProperty(globalThis, 'navigator', {
    configurable:true,
    value:{locks:new MemoryLockManager()},
  });
  try {
    await Promise.all([
      installMuiZip({archive, rootDirectory:firstHandle, metadata:{...metadata, packId:'tab-one', name:'Tab One'}}),
      installMuiZip({archive, rootDirectory:secondHandle, metadata:{...metadata, packId:'tab-two', name:'Tab Two'}}),
    ]);
  } finally {
    if (previousNavigator) Object.defineProperty(globalThis, 'navigator', previousNavigator);
    else delete globalThis.navigator;
  }
  const pyxis = root.children.get('pyxis-map');
  const slots = ['active-pack.0', 'active-pack.1']
    .map(name => pyxis.children.get(name))
    .filter(Boolean)
    .map(handle => decodeActiveSelection(handle.bytes));
  const newest = slots.sort((left, right) => right.generation - left.generation)[0];
  assert.equal(newest.generation, 2);
  assert.deepEqual(new Set(newest.packs.map(pack => pack.packId)), new Set(['tab-one', 'tab-two']));
});

test('an empty directory created during destination creation is never removed', async () => {
  const archive = storedZip([['2/1/1.png', PNG]]);
  const root = new MemoryDirectoryHandle();
  const pyxis = await root.getDirectoryHandle('pyxis-map', {create:true});
  class EmptyRaceDirectory extends MemoryDirectoryHandle {
    async getDirectoryHandle(name, options = {}) {
      if (name === metadata.packId && options.create && !this.raced) {
        this.raced = true;
        const injected = new MemoryDirectoryHandle(name, this.log);
        this.children.set(name, injected);
        return injected;
      }
      return super.getDirectoryHandle(name, options);
    }
  }
  const packs = new EmptyRaceDirectory('packs', root.log);
  pyxis.children.set('packs', packs);
  await assert.rejects(
    installMuiZip({
      archive,
      rootDirectory:root,
      metadata,
      onProgress: progress => { if (progress.phase === 'write') throw new Error('forced interruption'); },
    }),
    /forced interruption/i,
  );
  const raced = packs.children.get(metadata.packId);
  assert.ok(raced, 'concurrently created empty directory must remain');
  assert.deepEqual([...raced.children.keys()], []);
});

test('destination-creation race preserves a pre-existing pack directory', async () => {
  const archive = storedZip([['2/1/1.png', PNG]]);
  const root = new MemoryDirectoryHandle();
  const pyxis = await root.getDirectoryHandle('pyxis-map', {create:true});
  class RacingPacksDirectory extends MemoryDirectoryHandle {
    async getDirectoryHandle(name, options = {}) {
      if (name === metadata.packId && options.create && !this.raced) {
        this.raced = true;
        const injected = new MemoryDirectoryHandle(name, this.log);
        const manifest = new MemoryFileHandle('manifest.pmp', this.log);
        manifest.bytes = new Uint8Array([9, 8, 7]);
        injected.children.set('manifest.pmp', manifest);
        this.children.set(name, injected);
        return injected;
      }
      return super.getDirectoryHandle(name, options);
    }
  }
  const packs = new RacingPacksDirectory('packs', root.log);
  pyxis.children.set('packs', packs);
  await assert.rejects(
    installMuiZip({archive, rootDirectory:root, metadata}),
    /destination.*changed|created concurrently|not empty/i,
  );
  const raced = packs.children.get(metadata.packId);
  assert.deepEqual([...raced.children.get('manifest.pmp').bytes], [9, 8, 7]);
  assert.equal(raced.children.has('tiles'), false);
});

test('map-set capacity is checked before a ninth pack is published', async () => {
  const archive = storedZip([['2/1/1.png', PNG]]);
  const root = new MemoryDirectoryHandle();
  const pyxis = await root.getDirectoryHandle('pyxis-map', {create:true});
  const slot = await pyxis.getFileHandle('active-pack.0', {create:true});
  const writable = await slot.createWritable();
  await writable.write(encodeActiveMapSet({generation:8,mapSetId:'osm-bright',attribution:metadata.attribution,packs:
    Array.from({length:8}, (_,index) => ({packId:`pack-${index}`}))}));
  await writable.close();
  await assert.rejects(installMuiZip({archive,rootDirectory:root,metadata:{...metadata,packId:'pack-nine',name:'Pack Nine'}}), /8-pack/i);
  assert.equal(pyxis.children.has('packs'), false);
});

// Seed a card with a valid inherited composition: one published pack and a
// matching active slot, so preflight validation passes for the inherited
// pack before the new install runs.
async function seedInheritedCard(root, slotName, generation, inheritedPackId) {
  const style = getMuiStyleProfile('osm-bright');
  const seedMeta = {
    ...metadata, packId: inheritedPackId, name: 'Seed Pack',
    attribution: style.attribution, source: style.source, license: style.license,
  };
  const pyxis = await root.getDirectoryHandle('pyxis-map', {create: true});
  const packs = await pyxis.getDirectoryHandle('packs', {create: true});
  const seed = await packs.getDirectoryHandle(inheritedPackId, {create: true});
  const manifest = serializeIndexlessManifest(seedMeta, 1, 1, 1);
  const manifestHandle = await seed.getFileHandle('manifest.pmp', {create: true});
  const manifestWritable = await manifestHandle.createWritable();
  await manifestWritable.write(manifest);
  await manifestWritable.close();
  const slotHandle = await pyxis.getFileHandle(slotName, {create: true});
  const slotWritable = await slotHandle.createWritable();
  await slotWritable.write(encodeActiveMapSet({
    generation, mapSetId: 'osm-bright', attribution: style.attribution,
    packs: [{packId: inheritedPackId}],
  }));
  await slotWritable.close();
  return pyxis;
}

test('activation publishes the style record before the active slot', async () => {
  const root = new MemoryDirectoryHandle();
  const pyxis = await seedInheritedCard(root, 'active-pack.0', 5, 'seed-pack');
  await installMuiZip({
    archive: storedZip([['maps/osm-bright/2/1/1.png', PNG]]),
    rootDirectory: root,
    metadata: {...metadata, packId: 'pack-a'},
  });
  const styleWrite = root.log.findIndex(name => name === 'write:osm-bright.pmas');
  const slotWrite = root.log.findIndex(name => name === 'write:active-pack.1');
  assert.notEqual(styleWrite, -1, 'style record must be written');
  assert.notEqual(slotWrite, -1, 'active slot must be written');
  assert.ok(styleWrite < slotWrite, 'style PMAS must be written before the active slot');
});

test('a failed style write leaves every active slot untouched and is retriable', async () => {
  const style = getMuiStyleProfile('osm-bright');
  const root = new MemoryDirectoryHandle();
  const pyxis = await seedInheritedCard(root, 'active-pack.0', 5, 'seed-pack');
  const slot0Before = await fileAt(root, 'pyxis-map/active-pack.0');
  // A corrupt 3-byte style record: present (so the resume check sees a
  // file), but undecodable (so the planner ignores it).
  const mapSets = await pyxis.getDirectoryHandle('map-sets', {create: true});
  const styleHandle = await mapSets.getFileHandle('osm-bright.pmas', {create: true});
  const corruptWritable = await styleHandle.createWritable();
  await corruptWritable.write(new Uint8Array([0xff, 0x00, 0x13]));
  await corruptWritable.close();
  const corruptStyle = await fileAt(root, 'pyxis-map/map-sets/osm-bright.pmas');
  styleHandle.failOnWrite = true;

  await assert.rejects(
    installMuiZip({
      archive: storedZip([['maps/osm-bright/2/1/1.png', PNG]]),
      rootDirectory: root,
      metadata: {...metadata, packId: 'pack-a'},
    }),
    /activation failed; retry with the same ZIP and pack ID/i,
  );
  styleHandle.failOnWrite = false;

  // The published pack stays on the card (exact retry is possible), the
  // existing slot is byte-identical, and no slot was created or clobbered.
  assert.equal((await fileAt(root, 'pyxis-map/packs/pack-a/manifest.pmp')).length > 0, true);
  assert.equal(
    Buffer.from(await fileAt(root, 'pyxis-map/active-pack.0')).toString('hex'),
    Buffer.from(slot0Before).toString('hex'),
  );
  assert.equal(pyxis.children.has('active-pack.1'), false);
  // The failed style write left the original corrupt bytes in place.
  assert.equal(
    Buffer.from(await fileAt(root, 'pyxis-map/map-sets/osm-bright.pmas')).toString('hex'),
    Buffer.from(corruptStyle).toString('hex'),
  );

  // Exact retry converges: the corrupt style file is ignored by the
  // planner, the plan is re-derived from the untouched slot, and style +
  // slot end up at the same generation.
  const resumed = await installMuiZip({
    archive: storedZip([['maps/osm-bright/2/1/1.png', PNG]]),
    rootDirectory: root,
    metadata: {...metadata, packId: 'pack-a'},
  });
  assert.equal(resumed.resumed, true);
  const styleRecord = decodeActiveSelection(await fileAt(root, 'pyxis-map/map-sets/osm-bright.pmas'));
  const slot1 = decodeActiveSelection(await fileAt(root, 'pyxis-map/active-pack.1'));
  assert.equal(styleRecord.generation, slot1.generation);
  assert.deepEqual(slot1.packs.map(pack => pack.packId), ['pack-a', 'seed-pack']);
  assert.equal(
    Buffer.from(await fileAt(root, 'pyxis-map/active-pack.0')).toString('hex'),
    Buffer.from(slot0Before).toString('hex'),
  );
});

test('a failed slot write keeps the prior valid slot active and converges on retry', async () => {
  const root = new MemoryDirectoryHandle();
  const pyxis = await seedInheritedCard(root, 'active-pack.0', 5, 'seed-pack');
  const slot0Before = await fileAt(root, 'pyxis-map/active-pack.0');
  const slot1Handle = await pyxis.getFileHandle('active-pack.1', {create: true});
  slot1Handle.failOnWrite = true;

  await assert.rejects(
    installMuiZip({
      archive: storedZip([['maps/osm-bright/2/1/1.png', PNG]]),
      rootDirectory: root,
      metadata: {...metadata, packId: 'pack-a'},
    }),
    /activation failed; retry with the same ZIP and pack ID/i,
  );
  slot1Handle.failOnWrite = false;

  // The style record advanced, but the failed slot write left the prior
  // valid slot (active-pack.0) intact as the fallback the firmware can use.
  const styleRecord = decodeActiveSelection(await fileAt(root, 'pyxis-map/map-sets/osm-bright.pmas'));
  assert.equal(styleRecord.generation, 6);
  assert.equal(
    Buffer.from(await fileAt(root, 'pyxis-map/active-pack.0')).toString('hex'),
    Buffer.from(slot0Before).toString('hex'),
  );
  assert.equal((await fileAt(root, 'pyxis-map/active-pack.1')).length, 0);

  // Exact retry re-plans from the advanced style record and converges:
  // both records carry the same new generation.
  await installMuiZip({
    archive: storedZip([['maps/osm-bright/2/1/1.png', PNG]]),
    rootDirectory: root,
    metadata: {...metadata, packId: 'pack-a'},
  });
  const retryStyle = decodeActiveSelection(await fileAt(root, 'pyxis-map/map-sets/osm-bright.pmas'));
  const retrySlot = decodeActiveSelection(await fileAt(root, 'pyxis-map/active-pack.1'));
  assert.equal(retryStyle.generation, retrySlot.generation);
  assert.equal(retryStyle.generation, 7);
  assert.deepEqual(retrySlot.packs.map(pack => pack.packId), ['pack-a', 'seed-pack']);
  assert.equal(
    Buffer.from(await fileAt(root, 'pyxis-map/active-pack.0')).toString('hex'),
    Buffer.from(slot0Before).toString('hex'),
  );
});
