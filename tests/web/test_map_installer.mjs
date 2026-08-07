// Copyright (c) 2026 Pyxis contributors
// SPDX-License-Identifier: MIT

import assert from 'node:assert/strict';
import test from 'node:test';
import {
  decodeActiveSelection,
  encodeActiveMapSet,
  inspectMuiZip,
  installMuiZip,
  parseSparseManifest,
} from '../../docs/flasher/js/map-installer.js';

const PNG = Buffer.from(
  'iVBORw0KGgoAAAANSUhEUgAAAQAAAAEACAIAAADTED8xAAAA1UlEQVR4nO3BMQEAAADCoPVP7WULoAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACAGwEtAAHMpTgHAAAAAElFTkSuQmCC',
  'base64',
);

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

async function child(root, path) {
  let current = root;
  for (const part of path.split('/')) current = current.children.get(part);
  return current;
}

const metadata = {
  packId: 'overview',
  mapSetId: 'osm-bright',
  name: 'Overview',
  attribution: 'Map data (c) OpenStreetMap contributors',
  source: 'Coalition MUI OSM Bright user download',
  license: 'ODbL-1.0',
};

test('active map-set wire format matches the cross-language golden record', () => {
  const record = encodeActiveMapSet({generation:1,mapSetId:'osm-bright',attribution:'Map data attribution',packs:[
    {packId:'detail',rowSpans:[{zoom:2,y:1,xMinimum:1,xMaximum:1},{zoom:4,y:5,xMinimum:5,xMaximum:5}]},
    {packId:'state',rowSpans:[{zoom:2,y:1,xMinimum:1,xMaximum:2}]},
  ]});
  assert.equal(Buffer.from(record).toString('hex'), '504d415302006900010000000a6f736d2d627269676874144d61702064617461206174747269627574696f6e020664657461696c020002010000000100000001000000040500000005000000050000000573746174650100020100000001000000020000009de230d6');
  assert.deepEqual(decodeActiveSelection(record).packs.map(pack => pack.packId), ['detail','state']);
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
  assert.deepEqual(report.rowSpans, [
    {zoom: 2, y: 1, xMinimum: 1, xMaximum: 2},
    {zoom: 2, y: 2, xMinimum: 1, xMaximum: 1},
  ]);

  const root = new MemoryDirectoryHandle();
  const result = await installMuiZip({archive, rootDirectory: root, metadata});
  assert.equal(result.tileCount, 3);
  const pack = await child(root, 'pyxis-map/packs/overview');
  const manifest = pack.children.get('manifest.pmp').bytes;
  assert.equal(Buffer.from(manifest.subarray(0, 4)).toString('ascii'), 'PMPK');
  assert.equal(manifest[4], 2);
  const parsed = parseSparseManifest(manifest);
  assert.equal(parsed.tileCount, 3);
  assert.equal(parsed.rowSpans.length, 2);
  const selection = decodeActiveSelection((await child(root, 'pyxis-map/active-pack.0')).bytes);
  assert.equal(selection.version, 2);
  assert.equal(selection.mapSetId, 'osm-bright');
  assert.deepEqual(selection.packs.map(pack => pack.packId), ['overview']);
  assert.equal(selection.generation, 1);
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

test('rejects traversal, duplicate keys, unsupported compression, and malformed PNGs', async () => {
  await assert.rejects(inspectMuiZip(storedZip([['../2/1/1.png', PNG]])), /path|traversal/i);
  await assert.rejects(inspectMuiZip(storedZip([['2/1/1.png', PNG], ['2/1/1.png', PNG]])), /duplicate/i);
  const malformed = Buffer.from(PNG); malformed[12] ^= 1;
  await assert.rejects(inspectMuiZip(storedZip([['2/1/1.png', malformed]])), /PNG/i);
  const invalidTransparency = pngWithChunkBeforeIdat(PNG, 'tRNS', [0]);
  await assert.rejects(inspectMuiZip(storedZip([['2/1/1.png', invalidTransparency]])), /transparency/i);
  const badAdler = rewriteFirstIdat(PNG, payload => {const changed=Buffer.from(payload);changed[changed.length-1]^=1;return changed;});
  await assert.rejects(inspectMuiZip(storedZip([['2/1/1.png', badAdler]])), /checksum|zlib|deflate/i);
  const trailingDeflate = rewriteFirstIdat(PNG, payload => Buffer.concat([payload.subarray(0,-4),Buffer.from([0]),payload.subarray(-4)]));
  await assert.rejects(inspectMuiZip(storedZip([['2/1/1.png', trailingDeflate]])), /trailing|zlib|deflate/i);
});

test('removes only its receipt-owned unpublished pack after an interrupted write', async () => {
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
  assert.equal(packs.children.has('interrupted'), false);
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
    Array.from({length:8}, (_,index) => ({packId:`pack-${index}`,rowSpans:[{zoom:2,y:1,xMinimum:1,xMaximum:1}]}))}));
  await writable.close();
  await assert.rejects(installMuiZip({archive,rootDirectory:root,metadata:{...metadata,packId:'pack-nine',name:'Pack Nine'}}), /active map set/i);
  assert.equal(pyxis.children.has('packs'), false);
});
