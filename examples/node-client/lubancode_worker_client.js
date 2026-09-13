'use strict';
// LubanCode Worker 受控启动的最小 Node 客户端(应用Worker接入单 P4 参考实现)。
//
// 面向"外部应用常驻、一项独立研究起一只 app-server Worker"的部署形态
// (todos/应用Worker接入补齐_Agent与Skills装配_独立参数目录及恢复隔离.todo §一):
//   - 每个 Worker 一份专属 env:本客户端从宿主 env 出发,先摘掉残留的
//     应用根变量,再按本 Worker 的参数根/数据根构造——不修改宿主自己的
//     全局环境,也不重定义 HOME/USERPROFILE 冒充应用参数根(合同
//     docs/reference/capability-contract.md §13.1 铁律)。
//   - 模型凭据经参数根里的 config.json 交给 Worker,不进 argv、不进
//     宿主环境(§八:密钥不得出现在 argv)。
//   - 会话创建与输入都带 clientOperationId(协议 1.3 幂等受理面),
//     回执丢失可按原键重发找回,终态经 operation/read 复查(§九)。
//   - 收口分四步走:停受理(beginClose)→ 断调度(不再发 turn/start)→
//     排空(waitIdle 等在飞回合终态)→ 回收(shutdown 请求等进程自退;
//     超时兜底 terminateHard 硬杀)。
//
// 依赖:Node >= 18(只用 fs/http/child_process 标准件)。协议走 stdio
// app-server 的逐行 JSON-RPC;不含任何业务字段,内核协议与实现以
// docs/features/app-server/README.md 为准。
//
// 用法骨架:
//   const { LubancodeWorkerClient } = require('./lubancode_worker_client.js');
//   const worker = await LubancodeWorkerClient.spawn({
//     binary: '/path/to/lubancode',          // 必填
//     configRoot: '<临时参数根>',             // 必填:LUBANCODE_HOME 的值
//     dataRoot: '<临时数据根>',               // 必填:LUBANCODE_DATA_HOME 的值
//     managed: true,                          // LUBANCODE_MANAGED=1(托管档)
//     deploymentProfile: '<参数根>/deployment.json',
//     model: { baseUrl: 'https://…', model: '…', apiKey: '…', wire: 'anthropic' },
//   });
//   const init = await worker.initialize();   // 钉协议版本,不符即抛
//   const thread = await worker.startThread({ clientOperationId: 'CREATE-1', cwd });
//   const accepted = await worker.startTurn({ threadId, text, clientOperationId: 'OP-1' });
//   await worker.waitTurnCompleted(threadId, accepted.turnId);
//   const facts = await worker.readOperation({ threadId, clientOperationId: 'OP-1' });
//   await worker.beginClose();                // 停受理
//   await worker.waitIdle();                  // 排空
//   const exit = await worker.shutdown();     // 回收(退出码 0)
//   await worker.cleanupRoots();              // 临时根收尾(可选)

const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawn } = require('child_process');

// 客户端钉住的协议版本(§十一 固定版本兼容记录):initialize 回的
// protocolVersion 与此不符即抛错,不带着版本错配继续跑。
const PINNED_PROTOCOL_VERSION = '1.3';

class RpcError extends Error {
  constructor(code, message, data) {
    super(message);
    this.name = 'RpcError';
    this.code = code;
    this.data = data || null;
  }
}

class LubancodeWorkerClient {
  constructor(options) {
    this.options = options || {};
    if (!this.options.binary) {
      throw new Error('binary 必填:lubancode 可执行文件路径');
    }
    if (!this.options.configRoot) {
      throw new Error('configRoot 必填:应用参数根(LUBANCODE_HOME)');
    }
    if (!this.options.dataRoot) {
      throw new Error('dataRoot 必填:运行数据根(LUBANCODE_DATA_HOME)');
    }
    this.expectedProtocolVersion = this.options.expectedProtocolVersion || PINNED_PROTOCOL_VERSION;
    this.child = null;
    this.nextId = 1;
    this.pending = new Map(); // id -> {resolve, reject, timer}
    this.events = [];         // 全量事件账(断言/对账用)
    this.stdoutText = '';     // 完整 stdout 文本(凭据外泄扫描用)
    this.stderrText = '';
    this.inFlightTurns = new Set(); // 已受理未见 turn/completed 的 threadId
    this.closedForBusiness = false; // 停受理旗(beginClose 起)
    this.exitInfo = null;
    this.exitWaiters = [];
  }

  // ---- 材料准备与进程起落 ------------------------------------------------

  // 给这个 Worker 构造专属 env:宿主 env 快照出发,摘掉应用根变量残留,
  // 再按本 Worker 的根构造。envOverrides 最后叠(验收注入空值 env 用:
  // Node spawn 的 env 表里空串原样落 envblock,是跨进程传空值的唯一真路,
  // 见 capability-contract.md §13.1"Windows 空值语义")。
  buildEnv() {
    const env = Object.assign({}, process.env);
    for (const name of ['LUBANCODE_HOME', 'LUBANCODE_DATA_HOME', 'LUBANCODE_MANAGED',
      'LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS']) {
      delete env[name];
    }
    env.LUBANCODE_HOME = this.options.configRoot;
    env.LUBANCODE_DATA_HOME = this.options.dataRoot;
    if (this.options.managed) {
      env.LUBANCODE_MANAGED = '1';
    }
    if (this.options.neutralHome) {
      // 观察位:把 HOME/USERPROFILE 指到空临时树,验 Worker 对个人目录
      // 零读写(AW-02)。不是拿它冒充应用参数根——参数根只认上面三枚。
      env.HOME = this.options.neutralHome;
      env.USERPROFILE = this.options.neutralHome;
    }
    if (this.options.v3Sessions === true) {
      // e2e 钉 v3:operation/read 的 finalMessages 稳定正文走 v3 投影。
      env.LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS = '1';
    } else if (this.options.v3Sessions === false) {
      env.LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS = '0';
    }
    return Object.assign(env, this.options.envOverrides || {});
  }

  // 参数根里落 config.json:模型连接经参数根交给 Worker,不进 argv。
  writeModelConfig() {
    if (!this.options.model) {
      return;
    }
    fs.mkdirSync(this.options.configRoot, { recursive: true });
    const config = {
      wire: this.options.model.wire || 'anthropic',
      base_url: this.options.model.baseUrl,
      model: this.options.model.model,
    };
    if (this.options.model.apiKey) {
      config.api_key = this.options.model.apiKey;
    }
    fs.writeFileSync(path.join(this.options.configRoot, 'config.json'), JSON.stringify(config));
  }

  async start() {
    fs.mkdirSync(this.options.configRoot, { recursive: true });
    fs.mkdirSync(this.options.dataRoot, { recursive: true });
    this.writeModelConfig();
    // Node 的 spawn 是 command + args 两截(args 不含 argv[0],与 C++ 的
    // exec 风格 argv 数组不同):binary 只出现在 command 位。
    const args = ['app-server', '--yes'];
    if (this.options.deploymentProfile) {
      args.push('--app-server-profile', this.options.deploymentProfile);
    }
    const spawnOptions = {
      env: this.buildEnv(),
      stdio: ['pipe', 'pipe', 'pipe'],
    };
    if (this.options.cwd) {
      spawnOptions.cwd = this.options.cwd;
    }
    this.child = spawn(this.options.binary, args, spawnOptions);
    this.argv = [this.options.binary].concat(args); // 留档:凭据不进 argv 的断言底
    this.child.stdout.setEncoding('utf8');
    this.child.stderr.setEncoding('utf8');
    let stdoutBuffer = '';
    this.child.stdout.on('data', (chunk) => {
      this.stdoutText += chunk;
      stdoutBuffer += chunk;
      let newline = stdoutBuffer.indexOf('\n');
      while (newline >= 0) {
        const line = stdoutBuffer.slice(0, newline).replace(/\r$/, '');
        stdoutBuffer = stdoutBuffer.slice(newline + 1);
        this.handleLine(line);
        newline = stdoutBuffer.indexOf('\n');
      }
    });
    this.child.stderr.on('data', (chunk) => {
      this.stderrText += chunk;
    });
    this.child.on('error', (error) => {
      this.failAllPending(new Error('worker 进程起不来: ' + error.message));
    });
    this.child.on('exit', (code, signal) => {
      this.exitInfo = { code, signal };
      this.failAllPending(new Error('worker 进程已退出: code=' + code + ' signal=' + signal));
      for (const waiter of this.exitWaiters) {
        waiter(this.exitInfo);
      }
      this.exitWaiters = [];
    });
    return this;
  }

  // ---- 协议底座(逐行 JSON-RPC)-------------------------------------------

  handleLine(line) {
    if (!line) {
      return;
    }
    let message;
    try {
      message = JSON.parse(line);
    } catch (_) {
      return; // 坏行不炸,验收侧另有断言
    }
    if (message && message.id !== undefined &&
        (message.result !== undefined || message.error !== undefined)) {
      const waiter = this.pending.get(message.id);
      if (waiter) {
        this.pending.delete(message.id);
        clearTimeout(waiter.timer);
        if (message.error) {
          waiter.reject(new RpcError(message.error.code, message.error.message || '',
            message.error.data));
        } else {
          waiter.resolve(message.result);
        }
      }
      return;
    }
    if (message && message.method) {
      this.events.push(message);
    }
  }

  failAllPending(error) {
    for (const [, waiter] of this.pending) {
      clearTimeout(waiter.timer);
      waiter.reject(error);
    }
    this.pending.clear();
  }

  request(method, params, timeoutMs) {
    if (!this.child || this.exitInfo) {
      return Promise.reject(new Error('worker 进程不在'));
    }
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(method + ' 超时(' + (timeoutMs || 20000) + 'ms)'));
      }, timeoutMs || 20000);
      this.pending.set(id, { resolve, reject, timer });
      const line = JSON.stringify({ id, method, params: params || {} });
      this.child.stdin.write(line + '\n', (error) => {
        if (error) {
          this.pending.delete(id);
          clearTimeout(timer);
          reject(new Error(method + ' 写失败(断管): ' + error.message));
        }
      });
    });
  }

  notify(method, params) {
    if (!this.child || this.exitInfo) {
      throw new Error('worker 进程不在');
    }
    this.child.stdin.write(JSON.stringify({ method, params: params || {} }) + '\n');
  }

  waitForEvent(method, predicate, timeoutMs) {
    const deadline = Date.now() + (timeoutMs || 60000);
    const check = () => {
      for (let i = this.events.length - 1; i >= 0; --i) {
        const event = this.events[i];
        if (event.method === method && (!predicate || predicate(event.params))) {
          return Promise.resolve(event.params);
        }
      }
      if (Date.now() >= deadline) {
        return Promise.resolve(null);
      }
      return new Promise((resolve) => setTimeout(resolve, 50)).then(check);
    };
    return check();
  }

  countEvents(method, predicate) {
    return this.events.filter((event) => event.method === method &&
      (!predicate || predicate(event.params))).length;
  }

  waitExit(timeoutMs) {
    if (this.exitInfo) {
      return Promise.resolve(this.exitInfo);
    }
    return new Promise((resolve) => {
      const timer = setTimeout(() => resolve({ code: null, signal: null, timeout: true }), timeoutMs || 20000);
      const waiter = (info) => {
        clearTimeout(timer);
        resolve(info);
      };
      this.exitWaiters.push(waiter);
    });
  }

  // ---- 业务面(协议 1.3 的幂等受理闭合)------------------------------------

  // 握手:协议版本对不上就抛,不带病上岗(§十一 固定版本兼容)。
  async initialize(clientName) {
    const result = await this.request('initialize', { clientName: clientName || 'node-worker-client' });
    if (!result || result.protocolVersion !== this.expectedProtocolVersion) {
      throw new Error('协议版本不符:期望 ' + this.expectedProtocolVersion + ',回的是 ' +
        (result && result.protocolVersion));
    }
    this.notify('initialized');
    return result;
  }

  async startThread(params) {
    this.assertOpenForBusiness('thread/start');
    const result = await this.request('thread/start', params);
    return result;
  }

  async startTurn(params) {
    this.assertOpenForBusiness('turn/start');
    const result = await this.request('turn/start', params);
    if (result && result.duplicate !== true) {
      this.inFlightTurns.add(params.threadId);
    }
    return result;
  }

  async waitTurnCompleted(threadId, turnId, timeoutMs) {
    const completed = await this.waitForEvent('turn/completed',
      (params) => params.threadId === threadId && (!turnId || params.turnId === turnId),
      timeoutMs || 60000);
    if (completed) {
      this.inFlightTurns.delete(threadId);
    }
    return completed;
  }

  async readOperation(params) {
    return this.request('operation/read', params);
  }

  // ---- 收口四步:停受理 → 断调度 → 排空 → 回收(§十)-----------------------

  assertOpenForBusiness(method) {
    if (this.closedForBusiness) {
      throw new Error(method + ' 被拒:客户端已进收口(beginClose 之后停受理)');
    }
  }

  // 1) 停受理:收口 intent 一立,新会话/新回合不再发。
  beginClose() {
    this.closedForBusiness = true;
  }

  // 2) 断调度 + 3) 排空:等在飞回合都到终态(断调度由 beginClose 保证:
  // 没有新 turn/start,就没有新派发)。
  async waitIdle(timeoutMs) {
    const deadline = Date.now() + (timeoutMs || 60000);
    while (this.inFlightTurns.size > 0 && Date.now() < deadline) {
      await new Promise((resolve) => setTimeout(resolve, 50));
    }
    return this.inFlightTurns.size === 0;
  }

  // 4) 回收·正路:shutdown 请求 → 进程自退(期望退出码 0)。
  async shutdown(timeoutMs) {
    if (!this.child || this.exitInfo) {
      return this.exitInfo || { code: null, signal: null };
    }
    try {
      await this.request('shutdown', {}, timeoutMs || 20000);
    } catch (error) {
      // shutdown 响应没等到不等于没退:下面等退出,超时由调用方兜底。
    }
    return this.waitExit(timeoutMs || 20000);
  }

  // 4) 回收·兜底:硬杀。POSIX SIGKILL;Windows 上 child.kill() 走
  // TerminateProcess(同为不给清理机会的强杀语义)。
  async terminateHard() {
    if (!this.child || this.exitInfo) {
      return this.exitInfo || { code: null, signal: null };
    }
    if (process.platform === 'win32') {
      this.child.kill();
    } else {
      this.child.kill('SIGKILL');
    }
    return this.waitExit(20000);
  }

  // 孤儿收口路的另一头:模拟宿主消亡,直接关 stdin——stdio app-server
  // 读到 EOF 自行收线(正常退出,退出码 0)。
  async closeStdinAndWait(timeoutMs) {
    if (!this.child || this.exitInfo) {
      return this.exitInfo || { code: null, signal: null };
    }
    this.child.stdin.destroy();
    return this.waitExit(timeoutMs || 20000);
  }

  // 整套收口:停受理→排空→shutdown→仍不死才硬杀。返回实际退出信息。
  async close(options) {
    const opts = options || {};
    this.beginClose();
    if (opts.drain !== false) {
      await this.waitIdle(opts.idleTimeoutMs || 60000);
    }
    let exit = await this.shutdown(opts.shutdownTimeoutMs || 20000);
    if (!exit || exit.code !== 0) {
      exit = await this.terminateHard();
    }
    return exit;
  }

  // 临时根收尾:e2e 用。生产宿主按自己的留存策略处置数据根——会话账在
  // Worker 退出后仍是可查的持久事实,不该被客户端随手删。
  cleanupRoots() {
    for (const root of [this.options.configRoot, this.options.dataRoot, this.options.neutralHome]) {
      if (root) {
        try {
          fs.rmSync(root, { recursive: true, force: true });
        } catch (_) { /* 尽力 */ }
      }
    }
  }

  // 便捷工厂:起进程一步到位。
  static async spawn(options) {
    const client = new LubancodeWorkerClient(options);
    await client.start();
    return client;
  }
}

// e2e 场子常用:建一枚临时根。
function makeTempRoot(tag) {
  return fs.mkdtempSync(path.join(os.tmpdir(), 'lubancode-node-e2e-' + tag + '-'));
}

module.exports = { LubancodeWorkerClient, RpcError, makeTempRoot, PINNED_PROTOCOL_VERSION };
