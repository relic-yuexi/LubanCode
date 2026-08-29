// 配置层:启动参数、目录、视口与超时帽,只有这一处默认值。
// session、tools、transport 都从这里拿常量,谁也不许自造第二份——
// 两条入口(MCP 与直调)配出来的 Runtime 必须同一副牌。
//
// 禁指向用户日常 Chrome/Edge profile 的闸也在这层:配置一落地就拒,
// 浏览器还没起,谈不上污染。
'use strict';

const os = require('os');
const path = require('path');

function parseArgs(argv) {
  const out = {};
  for (let i = 0; i < argv.length; ++i) {
    const a = argv[i];
    const next = () => argv[++i];
    if (a === '--engine') out.engine = next();
    else if (a === '--headed') out.headless = false;
    else if (a === '--headless') out.headless = true;
    else if (a === '--profile') out.profile = next();
    else if (a === '--profile-name') out.profileName = next();
    else if (a === '--user-data-dir') out.userDataDir = next();
    else if (a === '--downloads-dir') out.downloadsDir = next();
    else if (a === '--viewport') out.viewport = next();
    else if (a === '--action-timeout-ms') out.actionTimeoutMs = Number(next());
    else if (a === '--journal-cap') out.journalCap = Number(next());
  }
  return out;
}

// args 是 parseArgs 的输出(测试宿主可手工构造同形对象);env 缺省取
// process.env。字段:engine/headless/profileMode/profileName/sessionId/
// userDataDir/downloadsDir/viewport/defaultActionTimeoutMs/maxActionTimeoutMs/
// journalCap(Console/Network 每页环形账帽)。
function buildConfig(args, env) {
  env = env || process.env;
  args = args || {};
  const sessionId = 's' + Date.now().toString(36) + Math.random().toString(36).slice(2, 6);
  return {
    engine: args.engine === 'webkit' ? 'webkit' : 'chromium',
    headless: args.headless !== undefined ? args.headless : Boolean(env.CI),
    profileMode: args.profile === 'persistent' ? 'persistent' : 'ephemeral',
    profileName: args.profileName || 'default',
    sessionId,
    userDataDir: args.userDataDir || defaultUserDataDir(args),
    downloadsDir: args.downloadsDir || path.join(homeBrowserDir(), 'downloads', sessionId),
    viewport: parseViewport(args.viewport),
    defaultActionTimeoutMs: Math.min(Math.max(args.actionTimeoutMs || 15000, 1000), 60000),
    maxActionTimeoutMs: 60000,
    // Console/Network journal 每页每账的容量帽(环形,丢最老明记 dropped)。
    journalCap: Math.min(Math.max(args.journalCap || 500, 10), 5000),
  };
}

function homeBrowserDir() {
  return path.join(os.homedir(), '.lubancode', 'browser');
}

function defaultUserDataDir(args) {
  const profileMode = args.profile === 'persistent' ? 'persistent' : 'ephemeral';
  const name = profileMode === 'persistent' ? (args.profileName || 'default') : 'ephemeral-' + process.pid;
  return path.join(homeBrowserDir(), 'profiles', name);
}

function parseViewport(raw) {
  const m = /^(\d{2,5})x(\d{2,5})$/.exec(raw || '');
  return m ? { width: Math.min(Number(m[1]), 3840), height: Math.min(Number(m[2]), 4320) } : { width: 1280, height: 720 };
}

function log(...parts) {
  process.stderr.write('[browser-mcp] ' + parts.join(' ') + '\n');
}

// 拒绝指向用户日常浏览器 profile:显式给的 user-data-dir 若与常见安装路径
// 重合,直接拒绝启动(单子 P1.2:禁止指向用户日常 Chrome/Edge profile)。
function rejectDailyProfileDir(dir) {
  const normalized = path.resolve(dir).toLowerCase();
  const suspects = [
    path.join(os.homedir(), 'appdata', 'local', 'google', 'chrome', 'user data'),
    path.join(os.homedir(), 'appdata', 'local', 'microsoft', 'edge', 'user data'),
    path.join(os.homedir(), 'library', 'application support', 'google', 'chrome'),
    path.join(os.homedir(), '.config', 'google-chrome'),
    path.join(os.homedir(), 'library', 'application support', 'mozilla', 'firefox'),
  ];
  for (const suspect of suspects) {
    const s = path.resolve(suspect).toLowerCase();
    if (normalized === s || normalized.startsWith(s + path.sep)) {
      return suspect;
    }
  }
  return null;
}

module.exports = { parseArgs, buildConfig, rejectDailyProfileDir, log };
