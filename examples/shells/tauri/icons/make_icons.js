#!/usr/bin/env node
// Tauri 壳图标生成器(多前端外壳单阶段 E)。零网络零依赖:Node 自带 zlib
// 手搓 PNG 与 BMP 内嵌的 ICO。图案是三横三竖交叠的九宫投影——对角交点
// 横条露头(绿),其余竖条在上(蓝),小尺寸下读得清。改了图案跑一遍
// `node icons/make_icons.js` 即重生成;产物(32x32.png/128x128.png/icon.ico)
// 随仓走,Tauri 打包要读它们。
'use strict';

const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

// ---------------------------------------------------------------------------
// PNG(RGBA, 8bit, 无交错)
// ---------------------------------------------------------------------------

const CRC_TABLE = (() => {
  const table = new Int32Array(256);
  for (let n = 0; n < 256; ++n) {
    let c = n;
    for (let k = 0; k < 8; ++k) {
      c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    }
    table[n] = c;
  }
  return table;
})();

function crc32(buf) {
  let c = 0xffffffff;
  for (let i = 0; i < buf.length; ++i) {
    c = CRC_TABLE[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  }
  return (c ^ 0xffffffff) >>> 0;
}

function chunk(type, data) {
  const out = Buffer.alloc(8 + data.length + 4);
  out.writeUInt32BE(data.length, 0);
  out.write(type, 4, 'ascii');
  data.copy(out, 8);
  out.writeUInt32BE(crc32(out.subarray(4, 8 + data.length)), 8 + data.length);
  return out;
}

function encodePng(width, height, rgba) {
  const raw = Buffer.alloc(height * (1 + width * 4));
  for (let y = 0; y < height; ++y) {
    const rowStart = y * (1 + width * 4);
    raw[rowStart] = 0; // filter: none
    rgba.copy(raw, rowStart + 1, y * width * 4, (y + 1) * width * 4);
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8;  // bit depth
  ihdr[9] = 6;  // color type: RGBA
  ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', ihdr),
    chunk('IDAT', zlib.deflateSync(raw, { level: 9 })),
    chunk('IEND', Buffer.alloc(0)),
  ]);
}

// ---------------------------------------------------------------------------
// ICO:BMP 条目(32bit BGRA + AND 掩码)。Vista 前的规矩,打包器最认。
// ---------------------------------------------------------------------------

function bmpEntry(size, rgbaGetter) {
  const pixels = Buffer.alloc(size * size * 4);
  for (let y = 0; y < size; ++y) {
    for (let x = 0; x < size; ++x) {
      const [r, g, b, a] = rgbaGetter(x, y);
      const flipped = size - 1 - y; // BMP 自底向上
      const at = (flipped * size + x) * 4;
      pixels[at] = b; pixels[at + 1] = g; pixels[at + 2] = r; pixels[at + 3] = a;
    }
  }
  const maskStride = Math.ceil(size / 32) * 4;
  const mask = Buffer.alloc(maskStride * size, 0); // 全 0:不透明靠 alpha 说话
  const header = Buffer.alloc(40);
  header.writeUInt32LE(40, 0);
  header.writeInt32LE(size, 4);
  header.writeInt32LE(size * 2, 8); // 高度翻倍:XOR + AND
  header.writeUInt16LE(1, 12);
  header.writeUInt16LE(32, 14);
  header.writeUInt32LE(pixels.length + mask.length, 20);
  return { data: Buffer.concat([header, pixels, mask]), size: size };
}

function encodeIco(entries) {
  const head = Buffer.alloc(6);
  head.writeUInt16LE(0, 0);
  head.writeUInt16LE(1, 2); // type: icon
  head.writeUInt16LE(entries.length, 4);
  const directory = Buffer.alloc(entries.length * 16);
  let offset = 6 + directory.length;
  entries.forEach((entry, i) => {
    const at = i * 16;
    directory.writeUInt8(entry.size >= 256 ? 0 : entry.size, at);
    directory.writeUInt8(entry.size >= 256 ? 0 : entry.size, at + 1);
    directory.writeUInt8(0, at + 2);
    directory.writeUInt8(0, at + 3);
    directory.writeUInt16LE(1, at + 4);      // planes
    directory.writeUInt16LE(32, at + 6);     // bpp
    directory.writeUInt32LE(entry.data.length, at + 8);
    directory.writeUInt32LE(offset, at + 12);
    offset += entry.data.length;
  });
  return Buffer.concat([head, directory].concat(entries.map((e) => e.data)));
}

// ---------------------------------------------------------------------------
// 图案:鲁班锁投影。三横三竖,size 无关的坐标归一化函数。
// ---------------------------------------------------------------------------

const BG = [24, 28, 34, 255];
const BAR_H = [46, 125, 50, 255];   // 横条:绿(参考前端的页签/epoch 同色系)
const BAR_V = [26, 120, 180, 255];  // 竖条:蓝

// 三条带的归一化区间 [起, 止),x/y 同规
const BANDS = [[0.10, 0.32], [0.39, 0.61], [0.68, 0.90]];

function pixel(x, y) {
  const u = x / 128;
  const v = y / 128;
  let h = -1;
  let vbar = -1;
  BANDS.forEach((band, i) => {
    if (u >= band[0] && u < band[1]) { h = i; }
    if (v >= band[0] && v < band[1]) { vbar = i; }
  });
  if (h < 0 || vbar < 0) {
    return (u >= 0.06 && u < 0.94 && v >= 0.06 && v < 0.94) ? [34, 40, 48, 255] : BG;
  }
  // 编织:对角交点横条在上,其余竖条在上——一眼看出是锁,不是格子。
  return h === vbar ? BAR_H : BAR_V;
}

function rgbaFor(size) {
  const buf = Buffer.alloc(size * size * 4);
  for (let y = 0; y < size; ++y) {
    for (let x = 0; x < size; ++x) {
      const c = pixel((x + 0.5) / size * 128, (y + 0.5) / size * 128);
      for (let i = 0; i < 4; ++i) {
        buf[(y * size + x) * 4 + i] = c[i];
      }
    }
  }
  return buf;
}

function pixelAt(size) {
  return (x, y) => pixel((x + 0.5) / size * 128, (y + 0.5) / size * 128);
}

const outDir = __dirname;
fs.writeFileSync(path.join(outDir, '32x32.png'), encodePng(32, 32, rgbaFor(32)));
fs.writeFileSync(path.join(outDir, '128x128.png'), encodePng(128, 128, rgbaFor(128)));
fs.writeFileSync(path.join(outDir, 'icon.ico'), encodeIco([bmpEntry(16, pixelAt(16)), bmpEntry(32, pixelAt(32)), bmpEntry(48, pixelAt(48))]));

// Android 壳的启动图标同一套图案(mipmap-mdpi 48、mipmap-hdpi 72)。
const androidRes = path.resolve(outDir, '..', '..', 'android', 'app', 'src', 'main', 'res');
fs.mkdirSync(path.join(androidRes, 'mipmap-mdpi'), { recursive: true });
fs.mkdirSync(path.join(androidRes, 'mipmap-hdpi'), { recursive: true });
fs.writeFileSync(path.join(androidRes, 'mipmap-mdpi', 'ic_launcher.png'), encodePng(48, 48, rgbaFor(48)));
fs.writeFileSync(path.join(androidRes, 'mipmap-hdpi', 'ic_launcher.png'), encodePng(72, 72, rgbaFor(72)));
console.log('icons: 32x32.png / 128x128.png / icon.ico 已生成(三横三竖九宫投影);android mipmap 48/72 已生成');
