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
  installMarkerIsFresh,
  installMuiZip,
  parseInstallMarker,
  parseSparseManifest,
  renewInstallMarker,
  resolveMuiStyleProfile,
  suggestMapIdentity,
  MapInstallerError,
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
  constructor(name, log) { this.name = name; this.kind = 'file'; this.bytes = new Uint8Array(); this.log = log; }
  async createWritable() {
    const handle = this;
    let pending = new Uint8Array();
    return {
      async write(value) { pending = new Uint8Array(value instanceof Blob ? await value.arrayBuffer() : value); },
      async close() { handle.bytes = pending; handle.log.push(`write:${handle.name}`); },
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
      // File System Access API: exclusive creation of an existing entry
      // fails (FileExistsError). The install-marker protocol relies on it.
      if (options.exclusive) {
        const error = new Error('The file already exists.');
        error.name = 'FileExistsError';
        throw error;
      }
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

test('rejects altered required style attribution before touching the SD root', async () => {
  const root = new MemoryDirectoryHandle();
  await assert.rejects(
    installMuiZip({archive:storedZip([['2/1/1.png', PNG]]),rootDirectory:root,
      metadata:{...metadata,attribution:'Map data'}}),
    /attribution|provenance/i,
  );
  assert.equal(root.children.has('pyxis-map'), false);
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
  await assert.rejects(installMuiZip({archive,rootDirectory:root,metadata:{...metadata,packId:'pack-nine',name:'Pack Nine'}}), /active map set/i);
  assert.equal(pyxis.children.has('packs'), false);
});


// --- Cross-producer install marker + commit-time revalidation (Greptile
// round-7 P1). The CLI builder claims the same on-disk marker token; the
// Web Locks name alone cannot coordinate with it. ---

const MARKER = '.pyxis-installing-web'; // this producer's own marker file

function freshMarker(owner = 'cli-0') {
  return new TextEncoder().encode(`PYXI 1 ${owner} ${Date.now()}`);
}

test('a fresh foreign install marker is refused before any record is written', async () => {
  const archive = storedZip([['2/1/1.png', PNG]]);
  // Both cross-producer files must refuse: the CLI builder's own file
  // and the legacy shared marker (old producers). Fresh markers are
  // never touched: the other installer is live.
  for (const foreignName of ['.pyxis-installing-cli', '.pyxis-installing']) {
    const root = new MemoryDirectoryHandle('sd');
    const pyxis = await root.getDirectoryHandle('pyxis-map', {create: true});
    const originalMarker = freshMarker('cli-0');
    const marker = new MemoryFileHandle(foreignName, root.log);
    marker.bytes = originalMarker;
    pyxis.children.set(foreignName, marker);

    await assert.rejects(
      installMuiZip({archive, rootDirectory: root, metadata}),
      /another map installer is already running/i,
    );
    // Nothing was published or activated; the foreign marker is untouched.
    assert.equal(pyxis.children.has(metadata.packId), false);
    assert.equal(pyxis.children.has('map-sets'), false);
    assert.equal(pyxis.children.has('active-pack.0'), false);
    assert.deepEqual([...marker.bytes], [...originalMarker]);
  }
});

test('a stale install marker is reclaimed and the marker is released on success', async () => {
  const archive = storedZip([['2/1/1.png', PNG]]);
  const root = new MemoryDirectoryHandle('sd');
  const pyxis = await root.getDirectoryHandle('pyxis-map', {create: true});
  const marker = new MemoryFileHandle(MARKER, root.log);
  marker.bytes = new TextEncoder().encode(`PYXI 1 cli-0 ${Date.now() - 24 * 60 * 60 * 1000}`);
  pyxis.children.set(MARKER, marker);

  const result = await installMuiZip({archive, rootDirectory: root, metadata});
  assert.equal(result.tileCount, 1);
  assert.equal(pyxis.children.has(MARKER), false);
});

test('activation aborts when a cross-producer commit lands before the record writes, then retry converges', async () => {
  const archive = storedZip([['2/1/1.png', PNG]]);
  const root = new MemoryDirectoryHandle('sd');
  const pyxis = await root.getDirectoryHandle('pyxis-map', {create: true});
  const mapSets = await pyxis.getDirectoryHandle('map-sets', {create: true});
  const seed = (directory, name, bytes) => {
    const file = new MemoryFileHandle(name, root.log);
    file.bytes = bytes;
    directory.children.set(name, file);
    return file;
  };
  const gen1 = encodeActiveMapSet({generation: 1, mapSetId: metadata.mapSetId, attribution: metadata.attribution, packs: [{packId: 'old'}]});
  seed(mapSets, `${metadata.mapSetId}.pmas`, gen1);
  const slot1 = seed(pyxis, 'active-pack.0', gen1);

  // Deterministic seam: prepareActiveMapSet now snapshots the raw slot
  // bytes first and derives the decoded records FROM that same snapshot
  // (round 11: no separate decode read), so active-pack.0 is read
  // plainly once per prepare pass -- the two prepare passes
  // (installMapPack, then activateMapSet) are followed by the
  // commit-time verifyActivationState re-read. That is the third plain
  // read overall; inject the racer's commit right before it, i.e.
  // between the snapshot derive and its revalidation.
  let plainSlotReads = 0;
  const realGetFileHandle = pyxis.getFileHandle.bind(pyxis);
  pyxis.getFileHandle = async (name, options) => {
    if (name === 'active-pack.0' && !(options && options.create)) plainSlotReads += 1;
    if (name === 'active-pack.0' && !(options && options.create) && plainSlotReads === 3) {
      const gen2 = encodeActiveMapSet({generation: 2, mapSetId: metadata.mapSetId, attribution: metadata.attribution, packs: [{packId: 'other'}, {packId: 'old'}]});
      slot1.bytes = gen2;
      seed(mapSets, `${metadata.mapSetId}.pmas`, gen2);
    }
    return realGetFileHandle(name, options);
  };
  try {
    await assert.rejects(
      installMuiZip({archive, rootDirectory: root, metadata}),
      /changed during installation/i,
    );
    // The raced state is intact (no record was written over it) and our
    // pack stays published under packs/ -- device-harmless until a record
    // names it.
    const packsDir = pyxis.children.get('packs');
    assert.ok(packsDir, 'packs directory exists');
    assert.equal(packsDir.children.has(metadata.packId), true);
    const raced = await decodeActiveSelection(pyxis.children.get('active-pack.0').bytes);
    assert.equal(raced.generation, 2);
    assert.deepEqual(raced.packs.map(pack => pack.packId), ['other', 'old']);
    const racedStyle = await decodeActiveSelection(mapSets.children.get(`${metadata.mapSetId}.pmas`).bytes);
    assert.equal(racedStyle.generation, 2);
    assert.equal(pyxis.children.has(MARKER), false, 'marker released even on abort');
  } finally {
    pyxis.getFileHandle = realGetFileHandle;
  }
  // The retry re-derives from the gen-2 state and converges to generation
  // 3 in the free slot (the racer's gen-2 record keeps active-pack.0).
  const retried = await installMuiZip({archive, rootDirectory: root, metadata});
  assert.equal(retried.tileCount, 1);
  const finalSlots = ['active-pack.0', 'active-pack.1']
    .map(name => pyxis.children.get(name))
    .filter(Boolean)
    .map(handle => decodeActiveSelection(handle.bytes));
  const newest = finalSlots.sort((left, right) => right.generation - left.generation)[0];
  assert.equal(newest.generation, 3);
  assert.deepEqual(newest.packs.map(pack => pack.packId), [metadata.packId, 'other', 'old']);
});


// --- Round 9: marker heartbeat. A long publication must renew its own
// marker so the claim never ages out mid-install; release and
// commit-time verification must still recognize the owner after the
// epoch has advanced.

test('renewing a live marker advances the epoch but keeps the owner', async () => {
  const root = new MemoryDirectoryHandle('sd');
  const pyxis = await root.getDirectoryHandle('pyxis-map', {create: true});
  const owner = `web-${Date.now().toString(16)}`;
  const token = `PYXI 1 ${owner} ${Date.now() - 16 * 60 * 1000}`; // 16 minutes old: past TTL
  const handle = await pyxis.getFileHandle(MARKER, {create: true, exclusive: true});
  const writable = await handle.createWritable({keepExistingData: false});
  await writable.write(new TextEncoder().encode(token));
  await writable.close();
  assert.equal(installMarkerIsFresh(Date.now() - 16 * 60 * 1000), false, '16 minutes old is past the TTL');
  // The heartbeat rewrites the marker with a fresh epoch, same owner.
  await renewInstallMarker(pyxis, owner);
  const renewed = await pyxis.getFileHandle(MARKER);
  const text = new TextDecoder('utf-8').decode(new Uint8Array(await (await renewed.getFile()).arrayBuffer()));
  const parsed = parseInstallMarker(text);
  assert.equal(parsed.owner, owner, 'renewal keeps the owner identity');
  assert.ok(parsed.epochMs >= Date.now() - 1000, 'renewal refreshes the epoch');
  assert.ok(installMarkerIsFresh(parsed.epochMs), 'a renewed marker is live again');
});


// --- Round 10 (Greptile): a heartbeat must never steal back a
// legitimately reclaimed marker. If our marker aged out and another
// producer reclaimed it, the renewal aborts and leaves the foreign
// claim intact; the install then fails and the user retries after the
// other installer finishes.
test('marker renewal refuses to steal a reclaimed claim', async () => {
  const root = new MemoryDirectoryHandle('sd');
  const pyxis = await root.getDirectoryHandle('pyxis-map', {create: true});
  const writeMarker = async text => {
    const writable = await (await pyxis.getFileHandle(MARKER, {create: true})).createWritable({keepExistingData: false});
    await writable.write(new TextEncoder().encode(text));
    await writable.close();
  };
  const readMarker = async () => {
    let handle = null; try { handle = await pyxis.getFileHandle(MARKER); } catch { return null; }
    const file = await handle.getFile();
    return new TextDecoder('utf-8').decode(new Uint8Array(await file.arrayBuffer()));
  };
  // Producer B reclaims after producer A's marker aged out.
  await writeMarker(`PYXI 1 cli-other ${Date.now()}`);
  // A's heartbeat must abort without touching B's claim.
  await assert.rejects(
    () => renewInstallMarker(pyxis, 'web-ours'),
    error => error.message.includes('reclaimed during installation'),
  );
  assert.equal(parseInstallMarker(await readMarker()).owner, 'cli-other');
  // Renewing as the same owner is fine.
  await renewInstallMarker(pyxis, 'cli-other');
  const renewed = parseInstallMarker(await readMarker());
  assert.equal(renewed.owner, 'cli-other');
  assert.ok(installMarkerIsFresh(renewed.epochMs));
  // Missing or corrupt marker: refused, never overwritten.
  await pyxis.removeEntry(MARKER);
  await assert.rejects(() => renewInstallMarker(pyxis, 'cli-other'));
  assert.equal(await readMarker(), null);
  await writeMarker('garbage');
  await assert.rejects(() => renewInstallMarker(pyxis, 'cli-other'));
});


// --- Round 11 (Greptile): the acquire-time cross check cannot see an
// installer that starts DURING our install. The per-tile heartbeat
// re-checks the cross producers, so a fresh CLI marker that appears
// mid-publication aborts the install before any record is written.
test('a fresh cross marker during installation aborts at renewal', async () => {
  const archive = storedZip([['2/1/1.png', PNG]]);
  const root = new MemoryDirectoryHandle('sd');
  let planted = false;
  await assert.rejects(
    installMuiZip({
      archive, rootDirectory: root, metadata,
      onProgress: state => {
        if (!planted && state.phase === 'write' && state.completed === 1) {
          planted = true;
          const pyxis = root.children.get('pyxis-map');
          const marker = new MemoryFileHandle('.pyxis-installing-cli', root.log);
          marker.bytes = freshMarker('cli-racer');
          pyxis.children.set('.pyxis-installing-cli', marker);
        }
      },
    }),
    /another map installer is already running/i,
  );
  const pyxis = root.children.get('pyxis-map');
  // Aborted before activation: no records; the racer's marker is
  // untouched (the other installer is live) and our own claim released.
  assert.equal(pyxis.children.has('active-pack.0'), false);
  assert.equal(pyxis.children.has('map-sets/osm-bright.pmas'), false);
  assert.equal(pyxis.children.has('.pyxis-installing-cli'), true);
  assert.equal(pyxis.children.has(MARKER), false, 'our marker released on abort');
});


// --- Round 12 (Greptile): the cross-marker reclaim is a check-then-act,
// and the owner can renew in the window. Reclaim must therefore be
// conditional: a token that became fresh between the original check and
// the deletion aborts the reclaim (the claim is live again), and the
// owner's commit-time verification must REQUIRE its own marker -- a
// missing claim is an abort, not a pass.

import {checkCrossInstallers, readInstallMarker, verifyActivationState} from '../../docs/flasher/js/map-installer.js';

test('reclaim aborts when the owner renews between check and delete', async () => {
  const root = new MemoryDirectoryHandle('sd');
  const pyxis = await root.getDirectoryHandle('pyxis-map', {create: true});
  const staleToken = `PYXI 1 cli-owner ${Date.now() - 16 * 60 * 1000}`;
  const freshToken = `PYXI 1 cli-owner ${Date.now()}`;
  const writeMarker = async text => {
    // Overwrite semantics (the seam simulates the owner's renewal
    // rewrite): no exclusive, since the file already holds the stale
    // token.
    const handle = await pyxis.getFileHandle('.pyxis-installing-cli', {create: true});
    const writable = await handle.createWritable({keepExistingData: false});
    await writable.write(new TextEncoder().encode(text));
    await writable.close();
  };
  await writeMarker(staleToken);
  // Deterministic seam: each marker read goes through
  // pyxis.getFileHandle. The first call is the original check (stale
  // token); the owner's renewal lands before the second call (the
  // conditional re-read), which must see the fresh token and abort
  // instead of deleting.
  let readCount = 0;
  const realGet = pyxis.getFileHandle.bind(pyxis);
  pyxis.getFileHandle = async (name, options) => {
    // Count plain reads only: the marker writes (create:true) are the
    // test's own seam writes, not reclaim reads.
    if (name === '.pyxis-installing-cli' && !options?.create) {
      readCount += 1;
      if (readCount === 2) await writeMarker(freshToken);
    }
    return realGet(name, options);
  };
  const realRemove = pyxis.removeEntry.bind(pyxis);
  let removed = false;
  pyxis.removeEntry = async name => { removed = true; return realRemove(name); };
  try {
    await assert.rejects(checkCrossInstallers(pyxis, true), MapInstallerError, 'renewal must abort the reclaim');
    assert.equal(removed, false, 'the renewed live claim must not be deleted');
    const surviving = await readInstallMarker(pyxis, '.pyxis-installing-cli');
    assert.equal(surviving, freshToken, 'the renewed token survives the reclaim attempt');
  } finally {
    pyxis.removeEntry = realRemove;
    pyxis.getFileHandle = realGet;
  }
});

test('commit-time verification aborts when the own marker is missing', async () => {
  const root = new MemoryDirectoryHandle('sd');
  const pyxis = await root.getDirectoryHandle('pyxis-map', {create: true});
  const mapSets = await pyxis.getDirectoryHandle('map-sets', {create: true});
  const owner = `web-${Date.now().toString(16)}`;
  const token = `PYXI 1 ${owner} ${Date.now()}`;
  // The claim existed at acquire time but a cross-reclaimer deleted it
  // after our last renewal: verify must abort, not proceed.
  const handle = await pyxis.getFileHandle(MARKER, {create: true, exclusive: true});
  const writable = await handle.createWritable({keepExistingData: false});
  await writable.write(new TextEncoder().encode(token));
  await writable.close();
  await pyxis.removeEntry(MARKER);
  await assert.rejects(
    verifyActivationState(pyxis, mapSets, 'osm-bright.pmas', token, {slotBytes: [null, null], styleBytes: null}),
    MapInstallerError, 'a reclaimed claim must abort the commit');
  // Round 12 (Greptile): an EXPIRED own marker (same owner, aged past
  // the TTL) is equally an abort: the claim is reclaimable right now,
  // so committing would race a reclaiming cross-producer.
  const writeMarker = async text => {
    const h = await pyxis.getFileHandle(MARKER, {create: true});
    const w = await h.createWritable({keepExistingData: false});
    await w.write(new TextEncoder().encode(text));
    await w.close();
  };
  await writeMarker(`PYXI 1 ${owner} ${Date.now() - 16 * 60 * 1000}`);
  await assert.rejects(
    verifyActivationState(pyxis, mapSets, 'osm-bright.pmas', token, {slotBytes: [null, null], styleBytes: null}),
    MapInstallerError, 'an expired claim must abort the commit');
  // A FRESH own marker (renewed just now) passes the marker check and
  // the unchanged slot/style re-reads.
  await writeMarker(token);
  await verifyActivationState(pyxis, mapSets, 'osm-bright.pmas', token, {slotBytes: [null, null], styleBytes: null});
});
