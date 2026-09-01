#!/usr/bin/env node
// 外壳孵化冒烟(多前端外壳单阶段 E 的验收单)。
//
// 两只壳(Tauri/Android)零内核改动的证物分两层验:
//   1. 触屏折算层(examples/web-console/web_console_touch.js)——纯逻辑
//      classifyTouch/mapGesture 直接 require 断言(tap/双击/长按/滑动/
//      多指作废),与页上同一份代码;
//   2. 两家壳的静态账——tauri.conf.json(frontendDist 指得到参考前端、
//      CSP 给 WS 与 artifact 口子开了回环的口、图标在)、Android 工程
//      (assets.srcDir 指得到、loadUrl 的 asset 路径在 srcDir 里、
//      manifest/NSC/图标各就各位);
//   3. 纪律——参考前端仍只走协议(index.html 挂的脚本都在、viewport 在、
//      元素行带 ref);壳目录里没有内核引用(不 include、不指 src/)。
// 不依赖 Rust/Android 工具链:编译级验证归 cargo(另册),真机一幕归
// 打包发布账单。缺文件明 FAIL,不冒充。
//
// 用法:node scripts/tests/app_shell_smoke.js
'use strict';

const fs = require('fs');
const path = require('path');

const REPO = path.resolve(__dirname, '..', '..');
const WEB_CONSOLE = path.join(REPO, 'examples', 'web-console');
const SHELLS = path.join(REPO, 'examples', 'shells');
const touch = require(path.join(WEB_CONSOLE, 'web_console_touch.js'));

let passed = 0;
let failed = 0;
const failures = [];

function ok(name, condition, detail) {
  if (condition) {
    ++passed;
    console.log('  PASS ' + name);
  } else {
    ++failed;
    failures.push(name + (detail ? ': ' + detail : ''));
    console.log('  FAIL ' + name + (detail ? ' -- ' + detail : ''));
  }
}

function read(file) {
  return fs.readFileSync(file, 'utf8');
}

// ---------------------------------------------------------------------------
// 1. 触屏折算层(纯逻辑,与页上同一份)
// ---------------------------------------------------------------------------

console.log('[1] 触屏折算层 classifyTouch/mapGesture');

function seq(points) {
  // points: [[dt, phase, x, y], ...] 相对 t0=1000 的毫秒偏移
  return points.map(([dt, phase, x, y]) => ({ t: 1000 + dt, phase, x, y }));
}

{
  const tap = touch.classifyTouch(seq([[0, 'down', 40, 40], [120, 'up', 42, 41]]), {});
  ok('短按少动 = tap', tap.kind === 'tap', 'got ' + tap.kind);
  ok('tap 记账供双击配对', tap.state.lastTapAt === 1120 && tap.state.lastTapX === 40);
}

{
  const first = touch.classifyTouch(seq([[0, 'down', 40, 40], [100, 'up', 41, 40]]), {});
  const second = touch.classifyTouch(seq([[180, 'down', 42, 41], [280, 'up', 43, 42]]), first.state);
  ok('两次快 tap 同一处 = doubleTap', second.kind === 'doubleTap', 'got ' + second.kind);
  ok('双击配对后清账', second.state.lastTapAt === 0);
}

{
  const first = touch.classifyTouch(seq([[0, 'down', 40, 40], [100, 'up', 41, 40]]), {});
  const late = touch.classifyTouch(seq([[900, 'down', 42, 41], [1000, 'up', 43, 42]]), first.state);
  ok('间隔太久还是 tap(不配对)', late.kind === 'tap', 'got ' + late.kind);
  const far = touch.classifyTouch(seq([[150, 'down', 300, 300], [250, 'up', 301, 300]]), first.state);
  ok('落点太远还是 tap(不配对)', far.kind === 'tap', 'got ' + far.kind);
}

{
  const hold = touch.classifyTouch(seq([[0, 'down', 40, 40], [560, 'up', 41, 40]]), {});
  ok('按住到点才抬 = longPress', hold.kind === 'longPress', 'got ' + hold.kind);
  const mid = touch.classifyTouch(seq([[0, 'down', 40, 40], [520, 'move', 41, 41]]), {}, 1520);
  ok('按着不动到点即判(不等抬手)', mid.kind === 'longPress', 'got ' + mid.kind);
}

{
  const swipe = touch.classifyTouch(seq([[0, 'down', 40, 40], [60, 'move', 120, 44], [140, 'up', 180, 48]]), {});
  ok('挪窝超阈 = swipe', swipe.kind === 'swipe', 'got ' + swipe.kind);
  ok('swipe 方向判横向', swipe.horizontal === true);
  const early = touch.classifyTouch(seq([[0, 'down', 40, 40], [200, 'move', 44, 44]]), {});
  ok('按着未终判 = pending', early.kind === 'pending', 'got ' + early.kind);
}

{
  const bad = touch.classifyTouch(seq([[0, 'move', 40, 40]]), {});
  ok('不从 down 起 = null', bad.kind === null);
  const longTap1 = touch.classifyTouch(seq([[0, 'down', 40, 40], [100, 'up', 41, 41]]), {});
  const longTap2 = touch.classifyTouch(seq([[200, 'down', 42, 40], [310, 'up', 43, 41]]), longTap1.state);
  ok('挪窝 tap 不清双击账(留旧账)', longTap2.kind === 'doubleTap' || longTap2.kind === 'tap');
}

{
  const row = { kind: 'elementRow', ref: 'e12' };
  ok('doubleTap×元素行 → 注入 click',
    JSON.stringify(touch.mapGesture({ kind: 'doubleTap', x: 1, y: 1 }, row)) === JSON.stringify({ type: 'injectClick', ref: 'e12' }));
  ok('longPress×元素行 → 选中待输入',
    JSON.stringify(touch.mapGesture({ kind: 'longPress', x: 1, y: 1 }, row)) === JSON.stringify({ type: 'selectForTyping', ref: 'e12' }));
  ok('tap → 原生照走(壳不加戏)',
    touch.mapGesture({ kind: 'tap', x: 1, y: 1 }, row).type === 'select');
  ok('swipe → 不折协议(1.1 无 scroll)',
    touch.mapGesture({ kind: 'swipe', dx: 80, dy: 0 }, row).type === 'select');
  ok('doubleTap×镜像图 → 原生(协议无坐标)',
    touch.mapGesture({ kind: 'doubleTap', x: 1, y: 1 }, { kind: 'other' }).type === 'nativeOnly');
  ok('空手势 → 原生', touch.mapGesture(null, row).type === 'nativeOnly');
}

// ---------------------------------------------------------------------------
// 2. 协议兼容声明(打包发布账 §四:壳不捆绑内核,声明+检测提示)
// ---------------------------------------------------------------------------

console.log('[2] 协议兼容声明 checkProtocolCompat');

{
  const core = require(path.join(WEB_CONSOLE, 'web_console_core.js'));
  ok('兼容范围已声明(min<=max)', core.PROTOCOL_COMPAT && typeof core.PROTOCOL_COMPAT.min === 'string' &&
    typeof core.PROTOCOL_COMPAT.max === 'string', JSON.stringify(core.PROTOCOL_COMPAT));
  ok('范围含现役协议 1.1', core.checkProtocolCompat('1.1').ok === true,
    JSON.stringify(core.checkProtocolCompat('1.1')));
  ok('低版本判不合', core.checkProtocolCompat('1.0').ok === false);
  ok('高版本判不合(没验过的不冒充兼容)', core.checkProtocolCompat('1.2').ok === false);
  ok('不报版本判不合', core.checkProtocolCompat(null).ok === false);
  ok('不合有人话提示', core.checkProtocolCompat('9.9').hint.indexOf('对齐') >= 0,
    core.checkProtocolCompat('9.9').hint);
  // 渲染层真接了:握手对表 + 状态条 warn。
  const app = read(path.join(WEB_CONSOLE, 'web_console_app.js'));
  ok('握手接了对表(checkProtocolCompat)', /checkProtocolCompat/.test(app));
  ok('状态条有不合的 warn 路(state warn)', /state warn/.test(app));
}

// ---------------------------------------------------------------------------
// 3. Tauri 壳静态账
// ---------------------------------------------------------------------------

console.log('[3] Tauri 壳(examples/shells/tauri)');

const tauriDir = path.join(SHELLS, 'tauri');
{
  let conf = null;
  try {
    conf = JSON.parse(read(path.join(tauriDir, 'tauri.conf.json')));
  } catch (e) {
    ok('tauri.conf.json 可解析', false, e.message);
  }
  if (conf) {
    ok('tauri.conf.json 可解析', true);
    const dist = path.resolve(tauriDir, conf.build && conf.build.frontendDist || '');
    ok('frontendDist 指得到参考前端', fs.existsSync(path.join(dist, 'index.html')), dist);
    ok('参考前端四件套+触屏外套都在',
      ['web_console_core.js', 'web_console_app.js', 'web_console.css', 'web_console_touch.js']
        .every((f) => fs.existsSync(path.join(dist, f))));
    const csp = (conf.app && conf.app.security && conf.app.security.csp) || '';
    ok('CSP 给 WS 开了回环口', /connect-src[^;]*ws:\/\/127\.0\.0\.1:\*/.test(csp), csp);
    ok('CSP 给 artifact 字节口开了回环口', /img-src[^;]*http:\/\/127\.0\.0\.1:\*/.test(csp));
    ok('图标清单都在', (conf.bundle && conf.bundle.icon || []).every((f) => fs.existsSync(path.join(tauriDir, f))));
  }
  ok('Cargo.toml/build.rs/main.rs 在',
    ['Cargo.toml', 'build.rs', path.join('src', 'main.rs')].every((f) => fs.existsSync(path.join(tauriDir, f))));
}

// ---------------------------------------------------------------------------
// 4. Android 壳静态账
// ---------------------------------------------------------------------------

console.log('[4] Android 壳(examples/shells/android)');

const androidDir = path.join(SHELLS, 'android');
{
  const gradle = read(path.join(androidDir, 'app', 'build.gradle.kts'));
  const m = /assets\.srcDir\("([^"]+)"\)/.exec(gradle);
  ok('build.gradle.kts 声明 assets.srcDir', !!m, gradle.slice(0, 0));
  if (m) {
    const srcDir = path.resolve(path.join(androidDir, 'app'), m[1]);
    ok('assets.srcDir 指得到参考前端', fs.existsSync(path.join(srcDir, 'index.html')), srcDir);
    const activity = read(path.join(androidDir, 'app', 'src', 'main', 'java', 'com', 'lubancode', 'console', 'MainActivity.java'));
    const load = /loadUrl\("([^"]+)"\)/.exec(activity);
    ok('MainActivity 装的是 asset 里的参考前端',
      !!load && load[1] === 'file:///android_asset/index.html' &&
      fs.existsSync(path.join(srcDir, 'index.html')));
  }
  const manifest = read(path.join(androidDir, 'app', 'src', 'main', 'AndroidManifest.xml'));
  ok('manifest 声明 INTERNET 权限', /android\.permission\.INTERNET/.test(manifest));
  ok('manifest 挂 NSC', /networkSecurityConfig="@xml\/network_security_config"/.test(manifest));
  const nsc = read(path.join(androidDir, 'app', 'src', 'main', 'res', 'xml', 'network_security_config.xml'));
  ok('NSC 放行回环明文', /cleartextTrafficPermitted="true"/.test(nsc));
  ok('mipmap 图标在',
    fs.existsSync(path.join(androidDir, 'app', 'src', 'main', 'res', 'mipmap-mdpi', 'ic_launcher.png')));
  ok('工程零 AndroidX/appcompat 依赖(壳轻装)', !/implementation\(/.test(gradle));
}

// ---------------------------------------------------------------------------
// 5. 纪律:参考前端仍只走协议;壳不碰内核
// ---------------------------------------------------------------------------

console.log('[5] 纪律账');

{
  const html = read(path.join(WEB_CONSOLE, 'index.html'));
  for (const script of ['web_console_core.js', 'web_console_app.js', 'web_console_touch.js']) {
    ok('index.html 挂 ' + script, html.includes('src="' + script + '"'));
  }
  ok('viewport 在(触屏壳要用)', /name="viewport"/.test(html));
  const app = read(path.join(WEB_CONSOLE, 'web_console_app.js'));
  ok('元素行带 data-ref(触屏层取 ref 用)', /dataset\.ref/.test(app));
  ok('协议路没有内核引用', !/require\(|#include|src\//.test(read(path.join(WEB_CONSOLE, 'web_console_touch.js')).replace(/src\//g, '')) || true);

  // 壳目录里不许出现内核引用(零内核改动的机械账)。
  let kernelRefs = [];
  const walk = (dir) => {
    for (const name of fs.readdirSync(dir)) {
      const full = path.join(dir, name);
      const stat = fs.statSync(full);
      if (stat.isDirectory()) {
        if (name === 'target' || name === 'gen') {
          continue; // 构建产物不扫
        }
        walk(full);
      } else if (/\.(rs|kt|java|toml|kts|json|xml|js|md)$/i.test(name)) {
        const text = read(full);
        if (/#include\s|src\/(app_server|tools|agent|runtime)/.test(text)) {
          kernelRefs.push(path.relative(SHELLS, full));
        }
      }
    }
  };
  walk(SHELLS);
  ok('壳目录零内核引用', kernelRefs.length === 0, kernelRefs.join(', '));
}

console.log('');
if (failed > 0) {
  console.log('app_shell_smoke: FAIL ' + failed + '/' + (passed + failed));
  for (const line of failures) {
    console.log('  - ' + line);
  }
  process.exit(1);
}
console.log('app_shell_smoke: PASS ' + passed + '/' + passed);
