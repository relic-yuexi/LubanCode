// 本地验收站(单子 P1.7):loopback server,端口动态分配,测试结束必收尸。
// 不依赖公网:静态页 + 三种小文件下载(text/png/zip)+ 永不结束的请求。
'use strict';

const crypto = require('crypto');
const fs = require('fs');
const http = require('http');
const path = require('path');

const SITE_DIR = path.resolve(__dirname, '../../tests/fixtures/browser_site');

// 最小合法 PNG(8x8 红块),zip 是含一个文本成员的真压缩包。
const PNG = Buffer.from(
  'iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAYAAADED76LAAAAFUlEQVR4nGP8z8Dwn4GBgYGJ' +
    'gYEBAAEoAQP8ZwLNAAAAAElFTkSuQmCC',
  'base64',
);
function makeZip() {
  // 手工攒一个最小 zip(本地文件头 + 中央目录 + 结尾记录),内容是 hello.txt。
  const content = Buffer.from('zip payload for lubancode browser site\n', 'utf8');
  const name = Buffer.from('hello.txt', 'ascii');
  const crc = crc32(content);
  const chunk = Buffer.alloc(30 + name.length + content.length);
  chunk.writeUInt32LE(0x04034b50, 0);
  chunk.writeUInt16LE(20, 4);
  chunk.writeUInt16LE(0, 6);
  chunk.writeUInt16LE(0, 8);
  chunk.writeUInt16LE(0, 10);
  chunk.writeUInt16LE(0x21, 12);  // 时间/日期占位
  chunk.writeUInt32LE(crc, 14);
  chunk.writeUInt32LE(content.length, 18);
  chunk.writeUInt32LE(content.length, 22);
  chunk.writeUInt16LE(name.length, 26);
  chunk.writeUInt16LE(0, 28);
  name.copy(chunk, 30);
  content.copy(chunk, 30 + name.length);
  const central = Buffer.alloc(46 + name.length);
  central.writeUInt32LE(0x02014b50, 0);
  central.writeUInt16LE(20, 4);
  central.writeUInt16LE(20, 6);
  central.writeUInt16LE(0, 8);
  central.writeUInt16LE(0, 10);
  central.writeUInt16LE(0, 12);
  central.writeUInt16LE(0x21, 14);
  central.writeUInt32LE(crc, 16);
  central.writeUInt32LE(content.length, 20);
  central.writeUInt32LE(content.length, 24);
  central.writeUInt16LE(name.length, 28);
  central.writeUInt32LE(0, 42);
  name.copy(central, 46);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0);
  end.writeUInt16LE(1, 8);
  end.writeUInt16LE(1, 10);
  end.writeUInt32LE(central.length, 12);
  end.writeUInt32LE(30 + name.length + content.length + central.length, 16);
  return Buffer.concat([chunk, central, end]);
}

function crc32(buffer) {
  let c;
  const table = [];
  for (let n = 0; n < 256; ++n) {
    c = n;
    for (let k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    table[n] = c >>> 0;
  }
  let crc = 0xffffffff;
  for (const byte of buffer) crc = table[(crc ^ byte) & 0xff] ^ (crc >>> 8);
  return (crc ^ 0xffffffff) >>> 0;
}

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.txt': 'text/plain; charset=utf-8',
};

function startSite() {
  const server = http.createServer((req, res) => {
    const url = new URL(req.url, 'http://127.0.0.1');
    if (url.pathname === '/api/ok') {
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end('{"ok":true}');
      return;
    }
    if (url.pathname === '/api/fail') {
      res.writeHead(500, { 'Content-Type': 'application/json' });
      res.end('{"ok":false}');
      return;
    }
    if (url.pathname === '/downloads/text') {
      res.writeHead(200, { 'Content-Type': 'text/plain; charset=utf-8', 'Content-Disposition': 'attachment; filename="note.txt"' });
      res.end('本地验收站的文本下载。\n');
      return;
    }
    if (url.pathname === '/downloads/png') {
      res.writeHead(200, { 'Content-Type': 'image/png', 'Content-Disposition': 'attachment; filename="shot.png"' });
      res.end(PNG);
      return;
    }
    if (url.pathname === '/downloads/zip') {
      res.writeHead(200, { 'Content-Type': 'application/zip', 'Content-Disposition': 'attachment; filename="pack.zip"' });
      res.end(makeZip());
      return;
    }
    if (url.pathname === '/forever') {
      // 永不结束的请求:测超时/取消,不 res.end()。
      res.writeHead(200, { 'Content-Type': 'image/png' });
      return;
    }
    const file = url.pathname === '/' ? '/index.html' : url.pathname;
    const target = path.join(SITE_DIR, path.normalize(file).replace(/^([/\\])+/, ''));
    if (!target.startsWith(SITE_DIR) || !fs.existsSync(target) || !fs.statSync(target).isFile()) {
      res.writeHead(404, { 'Content-Type': 'text/plain; charset=utf-8' });
      res.end('404');
      return;
    }
    res.writeHead(200, { 'Content-Type': MIME[path.extname(target)] || 'application/octet-stream' });
    res.end(fs.readFileSync(target));
  });
  return new Promise((resolve) => {
    server.listen(0, '127.0.0.1', () => resolve({ server, port: server.address().port }));
  });
}

module.exports = { startSite, PNG, makeZip };

if (require.main === module) {
  startSite().then(({ server, port }) => {
    process.stderr.write('site on http://127.0.0.1:' + port + '\n');
    process.on('SIGINT', () => server.close(() => process.exit(0)));
  });
}
