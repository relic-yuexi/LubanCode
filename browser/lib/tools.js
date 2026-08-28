// Tool adapter 层:把 BrowserSession 的能力翻成 MCP 工具面——名字、
// description、inputSchema、回执文案与 structuredContent。
//
// 边界(单子:内嵌浏览器调试工作台 阶段 1):
//   - 本层不持任何浏览器状态。页签、ref、下载、generation 一律走
//     session 的唯一账,不建第二份 page registry。
//   - 本层不碰 stdio、不碰 JSON-RPC——那是 transport 的事。
//   - 动作串行与墙钟在 callTool 里统一收口:所有工具调用排进
//     session.enqueue,总墙钟 = 单次墙钟 + 5s。
'use strict';

const { withDeadline } = require('./session');

// 只吃 session:adapter 不碰配置、不碰 Playwright 对象,所有常量经
// session.callTimeout 流转——这样它薄得下来,也换得动(App Server 的
// adapter 日后照同一面写)。
function createBrowserTools({ session }) {

  function pageMeta(m, extra) {
    return {
      page_id: m.pageId,
      url: m.url,
      title: '',
      generation: m.generation,
      ...(extra || {}),
    };
  }

  function textResult(text, structured) {
    return { content: [{ type: 'text', text }], structuredContent: structured || {}, isError: false };
  }

  const TOOLS = [
    {
      name: 'browser_status',
      description: '浏览器状态:engine、headed、profile、browser pid、打开的页数、下载目录。不开浏览器、不导航。',
      inputSchema: { type: 'object', properties: {} },
      handler: async () => {
        const s = await session.status();
        const text = 'browser: engine=' + s.engine
          + ' mode=' + (s.headless ? 'headless' : 'headed')
          + ' profile=' + s.profile + (s.profile === 'persistent' ? '(' + s.profileName + ')' : '')
          + ' launched=' + s.launched
          + (s.crashed ? ' crashed(' + s.crashReason + ')' : '')
          + ' pages=' + s.pages
          + ' downloads_dir=' + s.downloadsDir;
        return textResult(text, {
          engine: s.engine,
          headless: s.headless,
          profile: s.profile,
          profile_name: s.profileName,
          user_data_dir: s.userDataDir,
          launched: s.launched,
          crashed: s.crashed,
          crash_reason: s.crashReason,
          browser_pid: s.browserPid,
          pages: s.pages,
          downloads_dir: s.downloadsDir,
        });
      },
    },
    {
      name: 'browser_open',
      description: '打开一个 URL(只收 http/https/about:blank)。默认开新页;new_page=false 时在当前活动页导航。',
      inputSchema: {
        type: 'object',
        properties: {
          url: { type: 'string', description: '目标 URL(http/https/about:blank)' },
          new_page: { type: 'boolean', description: 'true=开新标签页(默认);false=当前页导航' },
          wait_until: { type: 'string', description: 'load|domcontentloaded|networkidle(默认 load)' },
          timeout_ms: { type: 'number', description: '本次动作墙钟(默认 15s,上限 60s)' },
        },
        required: ['url'],
      },
      handler: async (input) => {
        const m = await session.open(String(input.url || ''), {
          newPage: input.new_page,
          waitUntil: input.wait_until,
          timeoutMs: input.timeout_ms,
        });
        const summary = [
          '已打开 ' + m.url,
          '标题: ' + m.title,
          'page_id=' + m.pageId + ' generation=' + m.generation,
          m.httpStatus ? 'HTTP ' + m.httpStatus : '(无 HTTP 状态(about:blank 或缓存导航))',
        ].join('\n');
        return textResult(summary, pageMeta(m, { http_status: m.httpStatus }));
      },
    },
    {
      name: 'browser_snapshot',
      description: '语义快照(可访问性树,带 ref 标记):比整页 HTML 稳,模型按 ref 后续动作。ref 绑定当前 generation,导航后需重新快照。',
      inputSchema: {
        type: 'object',
        properties: {
          page_id: { type: 'string' },
          max_chars: { type: 'number', description: '快照字符帽(默认 20000)' },
          timeout_ms: { type: 'number' },
        },
      },
      handler: async (input) => {
        const m = await session.snapshot(input.page_id || null, { maxChars: input.max_chars, timeoutMs: input.timeout_ms });
        const header = 'snapshot_id=' + m.snapshotId + '\n[语义快照:主框架交互元素与标题;iframe 与 shadow DOM 首版不进快照(明报,不冒充);ref 绑定本页 generation,导航后重新快照]';
        return textResult(header + '\n' + m.text, pageMeta(m, { snapshot_id: m.snapshotId, truncated: m.truncated }));
      },
    },
    {
      name: 'browser_click',
      description: '点击快照里的控件(首选 ref)。点击前确认页面 generation;点击后等导航/DOM 稳定。ref 过期会明报 stale_ref。',
      inputSchema: {
        type: 'object',
        properties: {
          page_id: { type: 'string' },
          ref: { type: 'string', description: 'browser_snapshot 里的 eN ref' },
          snapshot_id: { type: 'string', description: 'ref 来自哪次快照(建议带上,导航后防 stale)' },
          timeout_ms: { type: 'number' },
        },
        required: ['ref'],
      },
      handler: async (input) => {
        const m = await session.click(input.page_id || null, String(input.ref || ''), { snapshotId: input.snapshot_id, timeoutMs: input.timeout_ms });
        const text = '已点击 ref=' + m.clickedRef
          + (m.navigated ? '(页面已导航,generation ' + m.generationBefore + ' -> ' + m.generation + ',旧 ref 全部失效)' : '(未导航,generation ' + m.generation + ')')
          + '\nURL: ' + m.url;
        return textResult(text, pageMeta(m, { clicked_ref: m.clickedRef, navigated: m.navigated }));
      },
    },
    {
      name: 'browser_type',
      description: '往输入框里填字。密码框的值不回显、不进日志。不支持自动读系统剪贴板。',
      inputSchema: {
        type: 'object',
        properties: {
          page_id: { type: 'string' },
          ref: { type: 'string' },
          snapshot_id: { type: 'string' },
          text: { type: 'string' },
          mode: { type: 'string', description: 'fill=整框替换(默认)| type=逐键输入' },
          press_enter: { type: 'boolean', description: '输入后按回车(提交表单用)' },
          timeout_ms: { type: 'number' },
        },
        required: ['ref', 'text'],
      },
      handler: async (input) => {
        const text = String(input.text ?? '');
        const m = await session.type(input.page_id || null, String(input.ref || ''), text, {
          snapshotId: input.snapshot_id,
          mode: input.mode,
          pressEnter: input.press_enter,
          timeoutMs: input.timeout_ms,
        });
        const shown = m.password ? '(密码框,值不回显)' : JSON.stringify(text);
        return textResult('已输入 ' + shown + (input.press_enter ? ' 并按回车' : '') + '\nURL: ' + m.url,
          pageMeta(m, { typed: m.typed, password: m.password }));
      },
    },
    {
      name: 'browser_select',
      description: '选 <select> 下拉框的一项:按 option 的 value 或可见文本(label)匹配。下拉别用 browser_type 发箭头键——那是往框里打字,选不动下拉。多选框(multiple)传数组一次选多项。',
      inputSchema: {
        type: 'object',
        properties: {
          page_id: { type: 'string' },
          ref: { type: 'string', description: 'browser_snapshot 里的 eN ref(须指向 <select>)' },
          snapshot_id: { type: 'string', description: 'ref 来自哪次快照(建议带上,导航后防 stale)' },
          value: { type: ['string', 'array'], items: { type: 'string' }, description: '按 option 的 value 属性匹配;数组=多选框一次选多项' },
          label: { type: ['string', 'array'], items: { type: 'string' }, description: '按 option 可见文本匹配(空白折叠后整串相等)。与 value 至少给一样;都给时按 value 优先' },
          timeout_ms: { type: 'number' },
        },
        required: ['ref'],
      },
      handler: async (input) => {
        const m = await session.select(input.page_id || null, String(input.ref || ''), {
          value: input.value,
          label: input.label,
          snapshotId: input.snapshot_id,
          timeoutMs: input.timeout_ms,
        });
        const shown = m.selected.map((o) => o.value + (o.label && o.label !== o.value ? '(' + o.label + ')' : '')).join(', ');
        return textResult('已选择 ' + m.selectedRef + ': ' + shown + '\nURL: ' + m.url,
          pageMeta(m, { selected_ref: m.selectedRef, selected: m.selected, multiple: m.multiple }));
      },
    },
    {
      name: 'browser_wait',
      description: '等一个条件成立:文本出现 / URL 含某段 / 固定毫秒(上限 60s)。不开放无限等待。',
      inputSchema: {
        type: 'object',
        properties: {
          page_id: { type: 'string' },
          for_text: { type: 'string', description: '等页面出现这段文本' },
          url_contains: { type: 'string', description: '等 URL 含这段' },
          ms: { type: 'number', description: '固定等待毫秒(<= 60000)' },
          timeout_ms: { type: 'number', description: '总墙钟(默认 15s,上限 60s)' },
        },
      },
      handler: async (input, token) => {
        const m = await session.wait(input.page_id || null, {
          forText: input.for_text,
          urlContains: input.url_contains,
          ms: input.ms,
          timeoutMs: input.timeout_ms,
          token,
        });
        let text;
        if (m.waitedFor === 'text') text = '文本已出现: ' + m.forText;
        else if (m.waitedFor === 'url') text = 'URL 已含 ' + m.urlContains + ': ' + m.url;
        else text = '等了 ' + m.ms + 'ms';
        return textResult(text, pageMeta(m, { waited_for: m.waitedFor }));
      },
    },
    {
      name: 'browser_tabs',
      description: '列出全部标签页:page_id、标题、URL、是否活动、generation。',
      inputSchema: { type: 'object', properties: {} },
      handler: async () => {
        const rows = await session.tabs();
        const body = rows.length === 0 ? '(没有页面)' : rows.map((r) => (r.active ? '*' : 'x') + ' ' + r.page_id + ' g' + r.generation + ' ' + r.title + ' -- ' + r.url).join('\n');
        return textResult(body, { tabs: rows });
      },
    },
    {
      name: 'browser_select_tab',
      description: '明确切换活动页(不靠"最后一页"猜)。',
      inputSchema: { type: 'object', properties: { page_id: { type: 'string' } }, required: ['page_id'] },
      handler: async (input) => {
        const m = await session.selectPage(String(input.page_id || ''), { timeoutMs: input.timeout_ms });
        return textResult('活动页已切到 ' + m.pageId + ': ' + m.url, pageMeta(m, {}));
      },
    },
    {
      name: 'browser_close_page',
      description: '关一页。关掉的 page_id 稳定报 closed,不偷偷复用 id。',
      inputSchema: { type: 'object', properties: { page_id: { type: 'string' } }, required: ['page_id'] },
      handler: async (input) => {
        const m = await session.closePage(String(input.page_id || ''), { timeoutMs: input.timeout_ms });
        return textResult('已关闭 ' + m.closedPageId + '(此后引用它明报 page_closed,不复用 id)', { closed_page_id: m.closedPageId, open_pages: m.openPages });
      },
    },
    {
      name: 'browser_screenshot',
      description: '截图(viewport / full_page / 元素)。回文本摘要 + image 块 + structuredContent 元数据(尺寸/字节)。图片走 MCP 富结果链,先落 artifact 再进历史。',
      inputSchema: {
        type: 'object',
        properties: {
          page_id: { type: 'string' },
          full_page: { type: 'boolean', description: '整页(默认 false=视口)' },
          ref: { type: 'string', description: '只截某个元素(可选)' },
          snapshot_id: { type: 'string' },
          timeout_ms: { type: 'number' },
        },
      },
      handler: async (input) => {
        const m = await session.screenshot(input.page_id || null, {
          fullPage: input.full_page,
          ref: input.ref,
          snapshotId: input.snapshot_id,
          timeoutMs: input.timeout_ms,
        });
        const content = [
          { type: 'text', text: '截图完成: ' + m.url + '\nPNG ' + m.buffer.length + '字节 sha256=' + m.sha256.slice(0, 12) + '…(图片字节在 image 块,宿主落 artifact 后模型与人都可看)' },
          { type: 'image', data: m.buffer.toString('base64'), mimeType: 'image/png' },
        ];
        return { content, structuredContent: pageMeta(m, { bytes: m.buffer.length, sha256: m.sha256, full_page: m.fullPage }), isError: false };
      },
    },
    {
      name: 'browser_downloads',
      description: '列下载账:状态、建议名、安全文件名、MIME、字节、sha256、落盘路径。默认隔离目录,可执行不自动运行、压缩包不自动解压。',
      inputSchema: { type: 'object', properties: {} },
      handler: async () => {
        const d = session.listDownloads();
        const body = d.rows.length === 0 ? '(还没有下载)' : d.rows.map((r) => r.id + ' ' + r.state + ' ' + r.filename + ' ' + r.bytes + 'B sha256=' + r.sha256.slice(0, 12) + '… -> ' + r.path).join('\n');
        return textResult(body, { downloads: d.rows, downloads_dir: d.downloadsDir });
      },
    },
  ];

  // tools/list 的回形:名字、描述、schema、title 与只读标注。
  function listTools() {
    return TOOLS.map((tool) => ({
      name: tool.name,
      description: tool.description,
      inputSchema: tool.inputSchema,
      title: tool.name.replace(/^browser_/, '').replace(/_/g, ' '),
      annotations: { readOnlyHint: tool.name === 'browser_status' || tool.name === 'browser_tabs' || tool.name === 'browser_downloads' || tool.name === 'browser_snapshot' },
    }));
  }

  // tools/call 的核心:未知工具回 isError 结果;已知工具排队串行 + 总墙钟,
  // 失败原样上抛,由宿主(MCP transport)决定取消账与错误回形。
  async function callTool(name, input, token) {
    const tool = TOOLS.find((t) => t.name === name);
    if (!tool) {
      return { content: [{ type: 'text', text: '未知工具: ' + name }], isError: true };
    }
    input = input || {};
    const result = await session.enqueue(() => withDeadline(
      Promise.resolve(tool.handler(input, token)),
      session.callTimeout({ timeoutMs: input.timeout_ms }) + 5000,
      '工具 ' + name,
    ));
    return result || { content: [], isError: false };
  }

  return { listTools, callTool };
}

module.exports = { createBrowserTools };
