// Copyright (c) 2026 Pyxis contributors
// SPDX-License-Identifier: MIT

import {Unzlib} from './vendor/fflate.js';
// Local-only, bounded MUI XYZ ZIP importer. This module deliberately has no
// network path: users select an archive and an SD-card directory themselves.

const MAX_ZOOM = 22;
const MAX_TILES = 100000;
const MAX_TOTAL_BYTES = 8 * 1024 * 1024 * 1024;
const MAX_TILE_BYTES = 384 * 1024;
const MAX_ROW_SPANS = 512;
const MAX_ZIP_ENTRIES = MAX_TILES + 64;
const MAX_CENTRAL_BYTES = 16 * 1024 * 1024;
const MAX_ZIP_NAME_BYTES = 255;
const MANIFEST_MAGIC = 0x4b504d50; // PMPK, little endian
const SELECTION_MAGIC = 0x53414d50; // PMAS, little endian
const textDecoder = new TextDecoder('utf-8', {fatal: true});
const textEncoder = new TextEncoder();
const installTails = new WeakMap();

async function withInstallLock(rootDirectory, operation) {
  const previous = installTails.get(rootDirectory) || Promise.resolve();
  let release;
  const gate = new Promise(resolve => { release = resolve; });
  const tail = previous.then(() => gate);
  installTails.set(rootDirectory, tail);
  await previous;
  try { return await operation(); }
  finally {
    release();
    if (installTails.get(rootDirectory) === tail) installTails.delete(rootDirectory);
  }
}

export class MapInstallerError extends Error {}
function fail(message) { throw new MapInstallerError(message); }
function u16(view, offset) { return view.getUint16(offset, true); }
function u32(view, offset) { return view.getUint32(offset, true); }
function putU16(view, offset, value) { view.setUint16(offset, value, true); }
function putU32(view, offset, value) { view.setUint32(offset, value >>> 0, true); }

export function crc32(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit++) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
  }
  return (~crc) >>> 0;
}

function checkedAscii(label, value, maximum) {
  if (typeof value !== 'string' || value.length === 0) fail(`${label} is required`);
  const bytes = textEncoder.encode(value);
  if (bytes.length > maximum || [...bytes].some(byte => byte < 0x20 || byte > 0x7e)) {
    fail(`${label} must be 1-${maximum} printable ASCII bytes`);
  }
  return bytes;
}

function validateMetadata(metadata) {
  if (!metadata || !/^[a-z0-9_-]{1,31}$/.test(metadata.packId || '')) {
    fail('Pack ID must match [a-z0-9_-]{1,31}');
  }
  return {
    packId: checkedAscii('Pack ID', metadata.packId, 31),
    name: checkedAscii('Name', metadata.name, 63),
    attribution: checkedAscii('Attribution', metadata.attribution, 127),
    source: checkedAscii('Source', metadata.source, 127),
    license: checkedAscii('License', metadata.license, 63),
  };
}

async function blobBytes(blob, start = 0, end = blob.size) {
  return new Uint8Array(await blob.slice(start, end).arrayBuffer());
}

function findEocd(bytes, absoluteStart) {
  for (let offset = bytes.length - 22; offset >= 0; offset--) {
    if (new DataView(bytes.buffer, bytes.byteOffset + offset, 4).getUint32(0, true) === 0x06054b50) {
      return absoluteStart + offset;
    }
  }
  fail('ZIP end record is missing');
}

async function parseZipDirectory(archive) {
  if (!(archive instanceof Blob) || archive.size < 22) fail('Selected ZIP is empty or truncated');
  const tailStart = Math.max(0, archive.size - 65557);
  const tail = await blobBytes(archive, tailStart);
  const eocdOffset = findEocd(tail, tailStart);
  const eocd = new DataView((await blobBytes(archive, eocdOffset, eocdOffset + 22)).buffer);
  if (u16(eocd, 4) !== 0 || u16(eocd, 6) !== 0) fail('Multi-disk ZIPs are unsupported');
  const diskCount = u16(eocd, 8);
  const count = u16(eocd, 10);
  const centralSize = u32(eocd, 12);
  const centralOffset = u32(eocd, 16);
  const commentLength = u16(eocd, 20);
  if (commentLength !== 0 || eocdOffset + 22 !== archive.size) fail('ZIP comments are unsupported');
  if (diskCount !== count || count === 0 || count === 0xffff || count > MAX_ZIP_ENTRIES ||
      centralSize === 0xffffffff || centralOffset === 0xffffffff) {
    fail('ZIP entry count exceeds the bounded limit or requires ZIP64');
  }
  if (centralOffset + centralSize !== eocdOffset || centralOffset + centralSize > archive.size) {
    fail('ZIP central directory bounds are invalid');
  }
  if (centralSize > MAX_CENTRAL_BYTES) fail('ZIP central directory exceeds the bounded limit');
  const central = await blobBytes(archive, centralOffset, centralOffset + centralSize);
  const view = new DataView(central.buffer, central.byteOffset, central.byteLength);
  const records = [];
  const names = new Set();
  let position = 0;
  for (let index = 0; index < count; index++) {
    if (position + 46 > central.length || u32(view, position) !== 0x02014b50) fail('ZIP central directory is malformed');
    const flags = u16(view, position + 8);
    const method = u16(view, position + 10);
    const checksum = u32(view, position + 16);
    const compressedSize = u32(view, position + 20);
    const size = u32(view, position + 24);
    const nameLength = u16(view, position + 28);
    const extraLength = u16(view, position + 30);
    const commentLength = u16(view, position + 32);
    const disk = u16(view, position + 34);
    const externalAttributes = u32(view, position + 38);
    const localOffset = u32(view, position + 42);
    const end = position + 46 + nameLength + extraLength + commentLength;
    if (end > central.length || nameLength === 0 || nameLength > MAX_ZIP_NAME_BYTES ||
        extraLength !== 0 || commentLength !== 0 || disk !== 0) {
      fail('ZIP entry metadata or extra fields are unsupported');
    }
    if ((flags & 1) !== 0) fail('Encrypted ZIP entries are unsupported');
    if ((flags & ~0x0808) !== 0) fail('ZIP entry uses unsupported flags');
    if (method !== 0 || compressedSize !== size) fail('Only stored MUI PNG ZIP entries are supported');
    if (size === 0 || size > MAX_TILE_BYTES) fail('ZIP tile exceeds the per-tile size limit');
    const unixMode = externalAttributes >>> 16;
    if ((externalAttributes & 0x10) !== 0) fail('ZIP directory entries are forbidden');
    if ((unixMode & 0xf000) === 0xa000) fail('ZIP symlinks are forbidden');
    if ((unixMode & 0xf000) !== 0 && (unixMode & 0xf000) !== 0x8000) fail('ZIP entry is not a regular file');
    let name;
    try { name = textDecoder.decode(central.subarray(position + 46, position + 46 + nameLength)); }
    catch { fail('ZIP filename is not valid UTF-8'); }
    if (name.startsWith('/') || name.includes('\\') || name.split('/').some(part => part === '' || part === '.' || part === '..')) {
      fail(`Unsafe ZIP path or traversal: ${name}`);
    }
    if (names.has(name)) fail(`Duplicate ZIP path: ${name}`);
    names.add(name);
    records.push({name, flags, method, checksum, compressedSize, size, localOffset});
    position = end;
  }
  if (position !== central.length) fail('ZIP central directory contains trailing records');
  return {records, centralOffset};
}

async function locateEntry(archive, record, centralOffset) {
  if (record.localOffset + 30 > centralOffset) fail(`ZIP local header is outside bounds: ${record.name}`);
  const headerBytes = await blobBytes(archive, record.localOffset, record.localOffset + 30);
  const header = new DataView(headerBytes.buffer, headerBytes.byteOffset, headerBytes.byteLength);
  const usesDescriptor = (record.flags & 8) !== 0;
  const localCrc = u32(header, 14), localCompressed = u32(header, 18), localSize = u32(header, 22);
  if (u32(header, 0) !== 0x04034b50 || u16(header, 6) !== record.flags || u16(header, 8) !== record.method ||
      (!usesDescriptor && (localCrc !== record.checksum || localCompressed !== record.compressedSize || localSize !== record.size)) ||
      (usesDescriptor && !((localCrc === 0 && localCompressed === 0 && localSize === 0) ||
                           (localCrc === record.checksum && localCompressed === record.compressedSize && localSize === record.size)))) {
    fail(`ZIP local header mismatch: ${record.name}`);
  }
  const nameLength = u16(header, 26);
  const extraLength = u16(header, 28);
  if (nameLength === 0 || nameLength > MAX_ZIP_NAME_BYTES || extraLength !== 0) {
    fail(`ZIP local extra fields are unsupported: ${record.name}`);
  }
  const nameBytes = await blobBytes(archive, record.localOffset + 30, record.localOffset + 30 + nameLength);
  let localName;
  try { localName = textDecoder.decode(nameBytes); } catch { fail('ZIP local filename is invalid'); }
  if (localName !== record.name) fail(`ZIP local filename mismatch: ${record.name}`);
  const dataOffset = record.localOffset + 30 + nameLength + extraLength;
  if (dataOffset + record.size > centralOffset) fail(`ZIP payload exceeds bounds: ${record.name}`);
  return {...record, dataOffset};
}

function adler32(bytes) {
  let a=1,b=0;
  for(let offset=0;offset<bytes.length;offset+=5552){const end=Math.min(bytes.length,offset+5552);for(let index=offset;index<end;index++){a+=bytes[index];b+=a;}a%=65521;b%=65521;}
  return ((b<<16)|a)>>>0;
}

async function inflatePngIdat(parts, maximum) {
  let compressedLength=0;for(const part of parts)compressedLength+=part.length;
  if(compressedLength<6||compressedLength>MAX_TILE_BYTES)fail('PNG zlib stream has an invalid length');
  const compressed=new Uint8Array(compressedLength);let compressedOffset=0;for(const part of parts){compressed.set(part,compressedOffset);compressedOffset+=part.length;}
  const cmf=compressed[0],flg=compressed[1];
  if((cmf&15)!==8||(cmf>>>4)>7||((cmf<<8)|flg)%31!==0||(flg&32)!==0)fail('PNG zlib header is invalid');
  const expectedAdler=new DataView(compressed.buffer,compressed.byteOffset+compressed.length-4,4).getUint32(0,false);
  let total=0;const output=[];let inflater;
  try {
    inflater=new Unzlib((part)=>{if(total+part.byteLength>maximum)fail('PNG decoded data exceeds its bounded size');total+=part.byteLength;output.push(part.slice());});
    const inputChunk=128;
    for(let offset=0;offset<compressed.length;offset+=inputChunk)inflater.push(compressed.subarray(offset,Math.min(compressed.length,offset+inputChunk)),offset+inputChunk>=compressed.length);
  } catch (error) {
    if (error instanceof MapInstallerError) throw error;
    fail('PNG zlib stream is invalid');
  }
  const residualBits=inflater.s?.p||0;
  if(inflater.p.length!==(residualBits===0?0:1))fail('PNG deflate stream contains trailing data');
  const joined=new Uint8Array(total);let offset=0;for(const part of output){joined.set(part,offset);offset+=part.length;}
  if(adler32(joined)!==expectedAdler)fail('PNG zlib checksum is invalid');
  return joined;
}

export async function validatePng(bytes, label = 'tile') {
  if (!(bytes instanceof Uint8Array) || bytes.length < 45 || bytes.length > MAX_TILE_BYTES) fail(`Invalid PNG: ${label}`);
  const signature = [137,80,78,71,13,10,26,10];
  if (!signature.every((byte, index) => bytes[index] === byte)) fail(`Invalid PNG signature: ${label}`);
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  let position = 8;
  let first = true;
  let sawIdat = false;
  let idatClosed = false;
  let sawIend = false;
  let sawPlte = false;
  let sawTrns = false;
  let sawPhys = false;
  let bitDepth = 0;
  let colorType = 0;
  let rowBytes = 0;
  let bytesPerPixel = 0;
  let paletteEntries = 0;
  const idat = [];
  while (position < bytes.length) {
    if (position + 12 > bytes.length) fail(`Invalid PNG structure: ${label}`);
    const length = view.getUint32(position, false);
    const typeBytes = bytes.subarray(position + 4, position + 8);
    const type = String.fromCharCode(...typeBytes);
    const end = position + 12 + length;
    if (end > bytes.length || !/^[A-Za-z]{4}$/.test(type) || (typeBytes[2] & 0x20)) fail(`Invalid PNG chunk: ${label}`);
    const payload = bytes.subarray(position + 8, position + 8 + length);
    const encodedCrc = view.getUint32(position + 8 + length, false);
    const crcInput = new Uint8Array(4 + length); crcInput.set(typeBytes); crcInput.set(payload, 4);
    if (crc32(crcInput) !== encodedCrc) fail(`Invalid PNG chunk CRC: ${label}`);
    if (first && (type !== 'IHDR' || length !== 13)) fail(`Invalid PNG IHDR: ${label}`);
    if (!first && type === 'IHDR') fail(`Duplicate PNG IHDR: ${label}`);
    if (!['IHDR','PLTE','tRNS','pHYs','IDAT','IEND'].includes(type)) fail(`Unsupported PNG chunk: ${label}`);
    if (sawIdat && type !== 'IDAT' && type !== 'IEND') idatClosed = true;
    if (type === 'IDAT' && idatClosed) fail(`Nonconsecutive PNG IDAT: ${label}`);
    if (type === 'IHDR') {
      const ihdr = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
      const width = ihdr.getUint32(0, false), height = ihdr.getUint32(4, false);
      bitDepth = payload[8]; colorType = payload[9];
      const depths = {0:[1,2,4,8],2:[8],3:[1,2,4,8],4:[8],6:[8]};
      if (width !== 256 || height !== 256 || !depths[colorType]?.includes(bitDepth) ||
          payload[10] !== 0 || payload[11] !== 0 || payload[12] !== 0) fail(`Unsupported 256x256 non-interlaced PNG: ${label}`);
      const channels = {0:1,2:3,3:1,4:2,6:4}[colorType];
      rowBytes = Math.ceil(width * channels * bitDepth / 8);
      bytesPerPixel = Math.max(1, Math.ceil(channels * bitDepth / 8));
      first = false;
    } else if (type === 'PLTE') {
      if (sawPlte || sawIdat || [0,4].includes(colorType) || length === 0 || length % 3 || length > 768) fail(`Invalid PNG palette: ${label}`);
      paletteEntries = length / 3; sawPlte = true;
      if (colorType === 3 && paletteEntries > (1 << bitDepth)) fail(`Invalid PNG palette: ${label}`);
    } else if (type === 'tRNS') {
      if (sawTrns || sawIdat) fail(`Invalid PNG transparency: ${label}`);
      if (colorType === 3) {
        if (!sawPlte || length === 0 || length > paletteEntries) fail(`Invalid PNG transparency: ${label}`);
      } else if (colorType === 0) {
        if (length !== 2) fail(`Invalid PNG transparency: ${label}`);
        if (bitDepth < 16 && new DataView(payload.buffer,payload.byteOffset,2).getUint16(0,false) >= 2 ** bitDepth) fail(`Invalid PNG transparency sample: ${label}`);
      } else if (colorType === 2) {
        if (length !== 6) fail(`Invalid PNG transparency: ${label}`);
        if (bitDepth < 16) {
          const transparent = new DataView(payload.buffer,payload.byteOffset,6);
          if ([0,2,4].some(offset => transparent.getUint16(offset,false) >= 2 ** bitDepth)) fail(`Invalid PNG transparency sample: ${label}`);
        }
      } else fail(`PNG color type cannot contain transparency: ${label}`);
      sawTrns = true;
    } else if (type === 'pHYs') {
      if (sawPhys || sawIdat || length !== 9 || payload[8] > 1) fail(`Invalid PNG pixel dimensions: ${label}`);
      sawPhys = true;
    } else if (type === 'IDAT') {
      if (colorType === 3 && !sawPlte) fail(`Indexed PNG lacks palette: ${label}`);
      sawIdat = true; idat.push(payload);
    } else if (type === 'IEND') {
      if (length !== 0 || !sawIdat || end !== bytes.length) fail(`Invalid PNG terminal chunk: ${label}`);
      sawIend = true;
    }
    position = end;
  }
  if (first || !sawIend) fail(`Invalid PNG terminal state: ${label}`);
  let decoded;
  try { decoded = await inflatePngIdat(idat, (rowBytes + 1) * 256); }
  catch (error) { fail(`Invalid PNG deflate stream: ${label}: ${error.message}`); }
  if (decoded.length !== (rowBytes + 1) * 256) fail(`Invalid PNG decoded length: ${label}`);
  let previous = new Uint8Array(rowBytes);
  for (let row = 0; row < 256; row++) {
    const base = row * (rowBytes + 1);
    const filter = decoded[base];
    if (filter > 4) fail(`Invalid PNG row filter: ${label}`);
    const current = new Uint8Array(rowBytes);
    for (let index = 0; index < rowBytes; index++) {
      const encoded = decoded[base + 1 + index];
      const left = index >= bytesPerPixel ? current[index - bytesPerPixel] : 0;
      const above = previous[index];
      const upperLeft = index >= bytesPerPixel ? previous[index - bytesPerPixel] : 0;
      let predictor = 0;
      if (filter === 1) predictor = left;
      else if (filter === 2) predictor = above;
      else if (filter === 3) predictor = Math.floor((left + above) / 2);
      else if (filter === 4) {
        const estimate = left + above - upperLeft;
        const pa = Math.abs(estimate - left), pb = Math.abs(estimate - above), pc = Math.abs(estimate - upperLeft);
        predictor = pa <= pb && pa <= pc ? left : pb <= pc ? above : upperLeft;
      }
      current[index] = (encoded + predictor) & 0xff;
    }
    if (colorType === 3) {
      for (let column = 0; column < 256; column++) {
        const bitOffset = column * bitDepth;
        const shift = 8 - bitDepth - (bitOffset % 8);
        const paletteIndex = (current[Math.floor(bitOffset / 8)] >>> shift) & ((1 << bitDepth) - 1);
        if (paletteIndex >= paletteEntries) fail(`PNG palette index is out of range: ${label}`);
      }
    }
    previous = current;
  }
  return {bitDepth, colorType};
}

function tilePath(name) {
  const match = /^(.*?)(\d+)\/(\d+)\/(\d+)\.png$/.exec(name);
  if (!match) fail(`Unexpected ZIP entry; expected XYZ PNG: ${name}`);
  const prefix = match[1];
  if (prefix && !/^maps\/[^/]+\/$/.test(prefix)) fail(`Unsupported MUI ZIP root: ${name}`);
  for (const value of match.slice(2)) if (!/^(0|[1-9]\d*)$/.test(value)) fail(`Noncanonical XYZ path: ${name}`);
  const [zoom, x, y] = match.slice(2).map(Number);
  if (zoom > MAX_ZOOM || x >= 2 ** zoom || y >= 2 ** zoom) fail(`XYZ coordinate out of range: ${name}`);
  return {prefix, zoom, x, y};
}

function makeRowSpans(entries) {
  const rows = new Map();
  for (const entry of entries) {
    const key = `${entry.zoom}/${entry.y}`;
    if (!rows.has(key)) rows.set(key, {zoom: entry.zoom, y: entry.y, xs: []});
    rows.get(key).xs.push(entry.x);
  }
  const spans = [];
  const ordered = [...rows.values()].sort((a,b) => a.zoom - b.zoom || a.y - b.y);
  for (const row of ordered) {
    row.xs.sort((a,b) => a-b);
    let start = row.xs[0], previous = start;
    for (const x of row.xs.slice(1)) {
      if (x !== previous + 1) { spans.push({zoom:row.zoom,y:row.y,xMinimum:start,xMaximum:previous}); start = x; }
      previous = x;
    }
    spans.push({zoom:row.zoom,y:row.y,xMinimum:start,xMaximum:previous});
    if (spans.length > MAX_ROW_SPANS) fail(`Sparse coverage exceeds ${MAX_ROW_SPANS} row spans`);
  }
  return spans;
}

async function validateZipLayout(archive, records, centralOffset) {
  const ordered = [...records].sort((a,b) => a.localOffset - b.localOffset);
  if (ordered[0].localOffset !== 0) fail('ZIP preambles are unsupported');
  for (let index = 0; index < ordered.length; index++) {
    const record = ordered[index];
    const dataEnd = record.dataOffset + record.size;
    const nextOffset = index + 1 < ordered.length ? ordered[index + 1].localOffset : centralOffset;
    if (record.localOffset < (index ? ordered[index - 1].dataOffset + ordered[index - 1].size : 0) || dataEnd > nextOffset) {
      fail(`Overlapping ZIP entries: ${record.name}`);
    }
    const descriptorLength = nextOffset - dataEnd;
    if ((record.flags & 8) === 0) {
      if (descriptorLength !== 0) fail(`Unexpected ZIP bytes after entry: ${record.name}`);
      continue;
    }
    if (descriptorLength !== 12 && descriptorLength !== 16) fail(`Invalid ZIP data descriptor: ${record.name}`);
    const descriptorBytes = await blobBytes(archive, dataEnd, nextOffset);
    const descriptor = new DataView(descriptorBytes.buffer, descriptorBytes.byteOffset, descriptorBytes.byteLength);
    const offset = descriptorLength === 16 ? 4 : 0;
    if ((descriptorLength === 16 && u32(descriptor, 0) !== 0x08074b50) ||
        u32(descriptor, offset) !== record.checksum ||
        u32(descriptor, offset + 4) !== record.compressedSize ||
        u32(descriptor, offset + 8) !== record.size) fail(`Invalid ZIP data descriptor: ${record.name}`);
  }
}

export async function inspectMuiZip(archive, {onProgress = () => {}} = {}) {
  const {records, centralOffset} = await parseZipDirectory(archive);
  const locatedRecords = [];
  for (const record of records) locatedRecords.push(await locateEntry(archive, record, centralOffset));
  await validateZipLayout(archive, locatedRecords, centralOffset);
  const entries = [];
  const tileKeys = new Set();
  let prefix = null;
  let totalBytes = 0;
  for (let index = 0; index < locatedRecords.length; index++) {
    const located = locatedRecords[index];
    const xyz = tilePath(located.name);
    if (prefix === null) prefix = xyz.prefix;
    if (prefix !== xyz.prefix) fail('ZIP contains multiple tile roots or styles');
    const key = `${xyz.zoom}/${xyz.x}/${xyz.y}`;
    if (tileKeys.has(key)) fail(`Duplicate tile key: ${key}`);
    tileKeys.add(key);
    totalBytes += located.size;
    if (entries.length >= MAX_TILES || totalBytes > MAX_TOTAL_BYTES) fail('ZIP exceeds map-pack quota');
    const bytes = await blobBytes(archive, located.dataOffset, located.dataOffset + located.size);
    if (crc32(bytes) !== located.checksum) fail(`ZIP CRC mismatch: ${located.name}`);
    await validatePng(bytes, located.name);
    entries.push({...located, ...xyz});
    onProgress({phase:'validate', completed:index + 1, total:records.length});
  }
  entries.sort((a,b) => a.zoom-b.zoom || a.x-b.x || a.y-b.y);
  const zooms = [...new Set(entries.map(entry => entry.zoom))].sort((a,b) => a-b);
  if (zooms.some((zoom,index) => zoom !== zooms[0] + index)) fail('ZIP zoom levels must be contiguous');
  return {entries, rowSpans:makeRowSpans(entries), tileCount:entries.length,
          totalBytes, minZoom:zooms[0], maxZoom:zooms.at(-1), prefix};
}

export function serializeSparseManifest(metadata, rowSpans, tileCount) {
  const fields = validateMetadata(metadata);
  if (!rowSpans.length || rowSpans.length > MAX_ROW_SPANS) fail('Invalid row-span count');
  let total = 0;
  for (let index = 0; index < rowSpans.length; index++) {
    const span = rowSpans[index];
    const world = 2 ** span.zoom;
    if (!Number.isInteger(span.zoom) || !Number.isInteger(span.y) || !Number.isInteger(span.xMinimum) ||
        !Number.isInteger(span.xMaximum) || span.zoom < 0 || span.zoom > MAX_ZOOM || span.y < 0 ||
        span.y >= world || span.xMinimum < 0 || span.xMinimum > span.xMaximum || span.xMaximum >= world) fail('Invalid row span');
    if (index && (span.zoom < rowSpans[index-1].zoom ||
        (span.zoom === rowSpans[index-1].zoom && span.y < rowSpans[index-1].y) ||
        (span.zoom === rowSpans[index-1].zoom && span.y === rowSpans[index-1].y && span.xMinimum <= rowSpans[index-1].xMaximum + 1))) fail('Noncanonical row spans');
    total += span.xMaximum - span.xMinimum + 1;
  }
  if (total !== tileCount || tileCount <= 0) fail('Manifest tile count mismatch');
  const stringsSize = Object.values(fields).reduce((sum, bytes) => sum + 1 + bytes.length, 0);
  const length = 16 + stringsSize + 8 + rowSpans.length * 13 + 4;
  const bytes = new Uint8Array(length); const view = new DataView(bytes.buffer);
  putU32(view, 0, MANIFEST_MAGIC); bytes[4] = 2; putU16(view, 6, 16); putU32(view, 8, length);
  let position = 16;
  for (const field of ['packId','name','attribution','source','license']) { const value=fields[field]; bytes[position++]=value.length; bytes.set(value,position); position+=value.length; }
  bytes[position++] = rowSpans[0].zoom; bytes[position++] = rowSpans.at(-1).zoom;
  putU16(view, position, rowSpans.length); position += 2; putU32(view, position, tileCount); position += 4;
  for (const span of rowSpans) { bytes[position++]=span.zoom; putU32(view,position,span.y); position+=4; putU32(view,position,span.xMinimum); position+=4; putU32(view,position,span.xMaximum); position+=4; }
  putU32(view, position, crc32(bytes.subarray(0, position)));
  return bytes;
}

export function parseSparseManifest(bytes) {
  bytes = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  if (bytes.length < 28 || bytes.length > 7100) fail('Manifest length is invalid');
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (u32(view,0)!==MANIFEST_MAGIC || bytes[4]!==2 || bytes[5]!==0 || u16(view,6)!==16 || u32(view,8)!==bytes.length || u32(view,12)!==0 || u32(view,bytes.length-4)!==crc32(bytes.subarray(0,-4))) fail('Manifest header or CRC is invalid');
  let position=16; const names=['packId','name','attribution','source','license']; const values={};
  for (const name of names) { if(position>=bytes.length-4) fail('Manifest string truncated'); const size=bytes[position++]; if(!size||position+size>bytes.length-4) fail('Manifest string truncated'); values[name]=textDecoder.decode(bytes.subarray(position,position+size)); position+=size; }
  if(position+8>bytes.length-4) fail('Manifest fields truncated'); const minZoom=bytes[position++], maxZoom=bytes[position++]; const count=u16(view,position); position+=2; const tileCount=u32(view,position); position+=4;
  if(!count||count>MAX_ROW_SPANS||position+count*13!==bytes.length-4) fail('Manifest span count invalid'); const rowSpans=[];
  for(let i=0;i<count;i++){ const zoom=bytes[position++], y=u32(view,position); position+=4; const xMinimum=u32(view,position); position+=4; const xMaximum=u32(view,position); position+=4; rowSpans.push({zoom,y,xMinimum,xMaximum}); }
  if(minZoom!==rowSpans[0].zoom||maxZoom!==rowSpans.at(-1).zoom) fail('Manifest zoom range invalid');
  return {...values,minZoom,maxZoom,tileCount,rowSpans};
}

function validateSelectionSpans(spans) {
  if (!Array.isArray(spans) || spans.length === 0 || spans.length > MAX_ROW_SPANS) fail('Invalid active map-set spans');
  let previous = null;
  for (const span of spans) {
    const world = 2 ** span.zoom;
    if (!Number.isInteger(span.zoom) || !Number.isInteger(span.y) || !Number.isInteger(span.xMinimum) ||
        !Number.isInteger(span.xMaximum) || span.zoom < 0 || span.zoom > MAX_ZOOM || span.y < 0 ||
        span.y >= world || span.xMinimum < 0 || span.xMinimum > span.xMaximum || span.xMaximum >= world ||
        (previous && (span.zoom < previous.zoom ||
          (span.zoom === previous.zoom && span.y < previous.y) ||
          (span.zoom === previous.zoom && span.y === previous.y && span.xMinimum <= previous.xMaximum + 1)))) {
      fail('Invalid or noncanonical active map-set spans');
    }
    previous = span;
  }
}

export function encodeActiveMapSet({generation, mapSetId, attribution, packs}) {
  const mapSet = checkedAscii('Map set ID', mapSetId, 31);
  const credit = checkedAscii('Attribution', attribution, 127);
  if (!/^[a-z0-9_-]{1,31}$/.test(mapSetId) || !Number.isInteger(generation) || generation < 1 ||
      generation > 0xffffffff || !Array.isArray(packs) || packs.length === 0 || packs.length > 8) {
    fail('Invalid active map set');
  }
  const seen = new Set(); let spanTotal = 0; let length = 12 + 1 + mapSet.length + 1 + credit.length + 1 + 4;
  const encodedPacks = packs.map(pack => {
    const id = checkedAscii('Pack ID', pack.packId, 31);
    if (!/^[a-z0-9_-]{1,31}$/.test(pack.packId) || seen.has(pack.packId)) fail('Invalid or duplicate active map-set pack');
    seen.add(pack.packId); validateSelectionSpans(pack.rowSpans); spanTotal += pack.rowSpans.length;
    if (spanTotal > MAX_ROW_SPANS) fail('Active map set exceeds the total row-span limit');
    length += 1 + id.length + 2 + pack.rowSpans.length * 13;
    return {id, pack};
  });
  const bytes = new Uint8Array(length); const view = new DataView(bytes.buffer);
  putU32(view,0,SELECTION_MAGIC); bytes[4]=2; putU16(view,6,length); putU32(view,8,generation);
  let position=12; bytes[position++]=mapSet.length; bytes.set(mapSet,position); position+=mapSet.length;
  bytes[position++]=credit.length; bytes.set(credit,position); position+=credit.length; bytes[position++]=encodedPacks.length;
  for (const {id,pack} of encodedPacks) {
    bytes[position++]=id.length; bytes.set(id,position); position+=id.length; putU16(view,position,pack.rowSpans.length); position+=2;
    for (const span of pack.rowSpans) {bytes[position++]=span.zoom;putU32(view,position,span.y);position+=4;putU32(view,position,span.xMinimum);position+=4;putU32(view,position,span.xMaximum);position+=4;}
  }
  putU32(view,position,crc32(bytes.subarray(0,position))); return bytes;
}

export function decodeActiveSelection(bytes) {
  bytes=bytes instanceof Uint8Array?bytes:new Uint8Array(bytes); if(bytes.length<16||bytes.length>7105)fail('Invalid active selection length'); const view=new DataView(bytes.buffer,bytes.byteOffset,bytes.byteLength);
  const version=bytes[4]; if(u32(view,0)!==SELECTION_MAGIC||![1,2].includes(version)||bytes[5]!==0||u16(view,6)!==bytes.length||u32(view,bytes.length-4)!==crc32(bytes.subarray(0,-4)))fail('Invalid active selection record');
  const generation=u32(view,8);if(!generation)fail('Invalid active selection generation');
  if(version===1){if(bytes.length!==48)fail('Invalid active selection length');const size=bytes[12];if(!size||size>31||[...bytes.subarray(13+size,44)].some(byte=>byte!==0))fail('Invalid active selection pack ID');const packId=textDecoder.decode(bytes.subarray(13,13+size));if(!/^[a-z0-9_-]{1,31}$/.test(packId))fail('Invalid active selection pack ID');return{version,packId,generation};}
  let position=12;const readText=(label,maximum,grammar=false)=>{if(position>=bytes.length-4)fail(`${label} is truncated`);const size=bytes[position++];if(!size||size>maximum||position+size>bytes.length-4)fail(`${label} is invalid`);const value=textDecoder.decode(bytes.subarray(position,position+size));position+=size;if([...textEncoder.encode(value)].some(byte=>byte<0x20||byte>0x7e)||(grammar&&!/^[a-z0-9_-]{1,31}$/.test(value)))fail(`${label} is invalid`);return value;};
  const mapSetId=readText('Map set ID',31,true),attribution=readText('Attribution',127);if(position>=bytes.length-4)fail('Active map-set pack count is truncated');const count=bytes[position++];if(!count||count>8)fail('Active map-set pack count is invalid');const packs=[],seen=new Set();let total=0;
  for(let packIndex=0;packIndex<count;packIndex++){const packId=readText('Pack ID',31,true);if(seen.has(packId))fail('Duplicate active map-set pack');seen.add(packId);if(position+2>bytes.length-4)fail('Active map-set span count is truncated');const spanCount=u16(view,position);position+=2;if(!spanCount||spanCount>MAX_ROW_SPANS||position+spanCount*13>bytes.length-4)fail('Active map-set span count is invalid');const rowSpans=[];for(let index=0;index<spanCount;index++){const zoom=bytes[position++],y=u32(view,position);position+=4;const xMinimum=u32(view,position);position+=4;const xMaximum=u32(view,position);position+=4;rowSpans.push({zoom,y,xMinimum,xMaximum});}validateSelectionSpans(rowSpans);total+=spanCount;if(total>MAX_ROW_SPANS)fail('Active map set exceeds the total row-span limit');packs.push({packId,rowSpans});}
  if(position!==bytes.length-4)fail('Active map-set record has trailing data');return{version,mapSetId,attribution,packs,generation};
}

async function getDirectory(parent,name,create=true){ return parent.getDirectoryHandle(name,{create}); }
async function sameDirectoryEntry(left,right){return left===right||(typeof left?.isSameEntry==='function'&&await left.isSameEntry(right));}
async function verifyNamedDirectory(parent,name,expected,allowedEntries){
  const named=await parent.getDirectoryHandle(name);
  if(!(await sameDirectoryEntry(expected,named)))fail(`SD destination changed during installation: ${name}`);
  const entries=[];for await(const [entryName] of expected.entries()){entries.push(entryName);if(entries.length>allowedEntries.length)break;}
  entries.sort();const allowed=[...allowedEntries].sort();
  if(entries.length!==allowed.length||entries.some((entry,index)=>entry!==allowed[index]))fail(`SD destination was created concurrently or is not empty: ${name}`);
}
async function writeVerified(parent,name,bytes){ const handle=await parent.getFileHandle(name,{create:true}); const writable=await handle.createWritable({keepExistingData:false}); try{await writable.write(bytes);await writable.close();}catch(error){try{await writable.abort();}catch{} throw error;} const actual=new Uint8Array(await (await handle.getFile()).arrayBuffer()); if(actual.length!==bytes.length) fail(`SD read-back mismatch: ${name}`); for(let index=0;index<bytes.length;index++) if(actual[index]!==bytes[index]) fail(`SD read-back mismatch: ${name}`); return handle; }
async function readSelection(pyxis,name){ let handle; try{handle=await pyxis.getFileHandle(name);}catch(error){if(error?.name==='NotFoundError')return null;throw error;} const bytes=new Uint8Array(await(await handle.getFile()).arrayBuffer()); try{return decodeActiveSelection(bytes);}catch{return null;} }
async function fileBytes(parent,name){const handle=await parent.getFileHandle(name);return new Uint8Array(await(await handle.getFile()).arrayBuffer());}
function equalBytes(left,right){if(left.length!==right.length)return false;for(let index=0;index<left.length;index++)if(left[index]!==right[index])return false;return true;}
async function removeOwnedPack(packs,packId,pack,receiptName,receipt,entries){
  try{
    const named=await packs.getDirectoryHandle(packId);if(!(await sameDirectoryEntry(pack,named)))return false;
    const actual=await fileBytes(pack,receiptName);if(!equalBytes(actual,receipt))return false;
    let tiles=null;try{tiles=await getDirectory(pack,'tiles',false);}catch(error){if(error?.name!=='NotFoundError')throw error;}
    if(tiles){
      const groups=new Map();for(const entry of entries){const zoom=String(entry.zoom),x=String(entry.x);if(!groups.has(zoom))groups.set(zoom,new Set());groups.get(zoom).add(x);let zoomDir,xDir;try{zoomDir=await getDirectory(tiles,zoom,false);xDir=await getDirectory(zoomDir,x,false);await xDir.removeEntry(`${entry.y}.png`);}catch(error){if(error?.name!=='NotFoundError')throw error;}}
      for(const [zoom,xs] of groups){let zoomDir;try{zoomDir=await getDirectory(tiles,zoom,false);}catch(error){if(error?.name==='NotFoundError')continue;throw error;}for(const x of xs){try{await zoomDir.removeEntry(x);}catch(error){if(error?.name!=='NotFoundError')return false;}}try{await tiles.removeEntry(zoom);}catch(error){if(error?.name!=='NotFoundError')return false;}}
      try{await pack.removeEntry('tiles');}catch(error){if(error?.name!=='NotFoundError')return false;}
    }
    try{await pack.removeEntry('manifest.pmp');}catch(error){if(error?.name!=='NotFoundError')return false;}
    for await(const [name] of pack.entries()){if(name!==receiptName)return false;}
    await pack.removeEntry(receiptName);return true;
  }catch{return false;}
}

async function prepareActiveMapSet(pyxis,metadata,rowSpans){
  const slots=await Promise.all([readSelection(pyxis,'active-pack.0'),readSelection(pyxis,'active-pack.1')]);
  if(slots[0]&&slots[1]&&slots[0].generation===slots[1].generation&&JSON.stringify(slots[0])!==JSON.stringify(slots[1]))fail('Conflicting active map-set records');
  const valid=slots.filter(Boolean);const highest=Math.max(0,...valid.map(slot=>slot.generation));if(highest===0xffffffff)fail('Active selection generation is exhausted');
  const current=valid.sort((left,right)=>right.generation-left.generation)[0];
  let packs=[];
  if(current?.version===2&&current.mapSetId===metadata.mapSetId){
    if(current.attribution!==metadata.attribution)fail('Installed map set attribution does not match');
    packs=current.packs.filter(pack=>pack.packId!==metadata.packId);
  }
  packs.unshift({packId:metadata.packId,rowSpans});
  const target=!slots[0]?'active-pack.0':!slots[1]?'active-pack.1':slots[0].generation<=slots[1].generation?'active-pack.0':'active-pack.1';
  const record=encodeActiveMapSet({generation:highest+1,mapSetId:metadata.mapSetId,attribution:metadata.attribution,packs});
  return {target,record,packs};
}

async function activateMapSet(pyxis,metadata,rowSpans){
  const {target,record}=await prepareActiveMapSet(pyxis,metadata,rowSpans);
  await writeVerified(pyxis,target,record);const selected=decodeActiveSelection(await fileBytes(pyxis,target));
  if(selected.version!==2||selected.mapSetId!==metadata.mapSetId||selected.packs[0].packId!==metadata.packId)fail('Active map-set verification failed');
  return {selectionFile:target,enabledPacks:selected.packs.map(pack=>pack.packId)};
}

async function verifyExistingPack(pack,archive,report,manifest){
  if(!equalBytes(await fileBytes(pack,'manifest.pmp'),manifest))fail('Existing pack does not match this ZIP and metadata');
  const tiles=await getDirectory(pack,'tiles',false);
  for(const entry of report.entries){const zoom=await getDirectory(tiles,String(entry.zoom),false);const x=await getDirectory(zoom,String(entry.x),false);const actual=await fileBytes(x,`${entry.y}.png`);const expected=await blobBytes(archive,entry.dataOffset,entry.dataOffset+entry.size);if(!equalBytes(actual,expected))fail(`Existing pack tile differs: ${entry.name}`);}
}

async function installMuiZipLocked({archive,rootDirectory,metadata,onProgress=()=>{}}){
  validateMetadata(metadata);if(!/^[a-z0-9_-]{1,31}$/.test(metadata.mapSetId||''))fail('Map set ID is invalid');if(!rootDirectory||rootDirectory.kind!=='directory')fail('Choose an SD-card directory');
  const report=await inspectMuiZip(archive,{onProgress});const manifest=serializeSparseManifest(metadata,report.rowSpans,report.tileCount);const pyxis=await getDirectory(rootDirectory,'pyxis-map');
  await prepareActiveMapSet(pyxis,metadata,report.rowSpans);const packs=await getDirectory(pyxis,'packs');
  let existing=null;try{existing=await packs.getDirectoryHandle(metadata.packId);}catch(error){if(error?.name!=='NotFoundError')throw error;}
  if(existing){
    let empty=true;for await(const _entry of existing.entries()){empty=false;break;}
    if(!empty){await verifyExistingPack(existing,archive,report,manifest);try{const active=await activateMapSet(pyxis,metadata,report.rowSpans);return{...report,manifestBytes:manifest.length,resumed:true,...active};}catch(error){throw new Error(`Map pack is installed and verified, but activation failed: ${error.message}`);}}
  }
  const pack=existing||await getDirectory(packs,metadata.packId);await verifyNamedDirectory(packs,metadata.packId,pack,[]);let published=false;const receipt=new Uint8Array(16);crypto.getRandomValues(receipt);const receiptName=`.pyxis-install-owner-${[...receipt].map(byte=>byte.toString(16).padStart(2,'0')).join('')}`;
  try{
    await writeVerified(pack,receiptName,receipt);await verifyNamedDirectory(packs,metadata.packId,pack,[receiptName]);
    const tiles=await getDirectory(pack,'tiles');
    for(let index=0;index<report.entries.length;index++){const entry=report.entries[index];const zoom=await getDirectory(tiles,String(entry.zoom));const x=await getDirectory(zoom,String(entry.x));const bytes=await blobBytes(archive,entry.dataOffset,entry.dataOffset+entry.size);if(crc32(bytes)!==entry.checksum)fail(`ZIP changed during installation: ${entry.name}`);await validatePng(bytes,entry.name);await writeVerified(x,`${entry.y}.png`,bytes);onProgress({phase:'write',completed:index+1,total:report.tileCount});}
    await writeVerified(pack,'manifest.pmp',manifest);parseSparseManifest(await fileBytes(pack,'manifest.pmp'));published=true;
    try{await pack.removeEntry(receiptName);}catch(error){throw new Error(`Map pack is installed and verified, but its ownership receipt could not be removed: ${error.message}`);}
    try{const active=await activateMapSet(pyxis,metadata,report.rowSpans);return{...report,manifestBytes:manifest.length,resumed:false,...active};}
    catch(error){throw new Error(`Map pack is installed and verified, but activation failed; retry with the same ZIP and pack ID: ${error.message}`);}
  }catch(error){
    if(!published&&!(await removeOwnedPack(packs,metadata.packId,pack,receiptName,receipt,report.entries))){
      throw new Error(`${error.message}; the incomplete pack could not be safely removed because its ownership receipt was missing or cleanup failed`);
    }
    throw error;
  }
}

export async function installMuiZip(arguments_) {
  const rootDirectory = arguments_?.rootDirectory;
  if (!rootDirectory || (typeof rootDirectory !== 'object' && typeof rootDirectory !== 'function')) {
    return installMuiZipLocked(arguments_ || {});
  }
  return withInstallLock(rootDirectory, () => installMuiZipLocked(arguments_));
}
