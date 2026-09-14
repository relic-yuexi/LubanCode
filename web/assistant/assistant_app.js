// LubanCode 助理主界面——页面逻辑(W1)。
//
// 纪律(单 §五/§六):
//   - 页面只是订阅与投影:真账在服务端(SessionService/V3),刷新后凭
//     thread/list + thread/read 恢复,localStorage 不当账本(sessionStorage
//     只放"上次在看哪场"这一枚指路签);
//   - 凭据不进 localStorage/URL(bootstrap fragment 交换完立即清);
//   - 消息按不可信文本渲染(textContent,不 innerHTML);
//   - 断线不等于停止:WS 断了显示"已断线",在跑任务照跑,重连后补账。
//
// 渲染模型:transcript 是追加式流水(history 条目 + 实时 item 共用一张
// 表,按 itemId 对账);整段重画只在换场时发生。
'use strict';

(function () {
  const core = window.LubanAssistant;

  // ---- DOM ----
  const el = function (id) { return document.getElementById(id); };
  const connState = el('conn-state');
  const assistantMeta = el('assistant-meta');
  const transcript = el('transcript');
  const threadList = el('thread-list');
  const threadIdLabel = el('thread-id-label');
  const turnStatusLabel = el('turn-status-label');
  const composer = el('composer');
  const sayInput = el('say-input');
  const sendButton = el('send-button');
  const interruptButton = el('interrupt-button');
  const newThreadButton = el('new-thread-button');
  const noticeOverlay = el('notice-overlay');
  const noticeTitle = el('notice-title');
  const noticeBody = el('notice-body');
  const noticeAction = el('notice-action');
  const configForm = el('config-form');
  const configStatusLine = el('config-status-line');
  const configSaveResult = el('config-save-result');
  const configTestResult = el('config-test-result');
  const stopAssistantButton = el('stop-assistant-button');

  let channel = null;
  let takeoverArmed = false; // 显式接管旗(§六:接管只换控制连接)
  let currentThreadId = '';
  let turnRunning = false;
  let answeredApprovals = new Set();

  // ---- 流水表:append-only;itemRows 把实时 item 的 <li> 与文本节点
  // 钉住,delta 只改文本不重画。 ----
  let itemRows = new Map(); // itemId -> {li, textNode}
  function resetTranscript() {
    transcript.textContent = '';
    itemRows = new Map();
  }

  function appendBubble(kind, label, text) {
    const li = document.createElement('li');
    li.className = 'msg ' + kind;
    if (label) {
      const name = document.createElement('span');
      name.className = 'label';
      name.textContent = label;
      li.appendChild(name);
    }
    const body = document.createElement('span');
    body.textContent = text || '';
    li.appendChild(body);
    transcript.appendChild(li);
    transcript.scrollTop = transcript.scrollHeight;
    return body;
  }

  function appendApprovalRow(params) {
    if (!params || answeredApprovals.has(params.requestId)) return;
    appendBubble('tool', '等审批', (params.toolName || '工具') + ':' +
      String(params.summary || params.risk || '') + '(批准/拒绝按钮在下方)');
    const row = document.createElement('div');
    row.className = 'row';
    const accept = document.createElement('button');
    accept.type = 'button';
    accept.textContent = '批准';
    accept.onclick = function () {
      answeredApprovals.add(params.requestId);
      channel.answerServerRequest(params.requestId, { decision: 'accept' });
      row.remove();
    };
    const decline = document.createElement('button');
    decline.type = 'button';
    decline.textContent = '拒绝';
    decline.onclick = function () {
      answeredApprovals.add(params.requestId);
      channel.answerServerRequest(params.requestId, { decision: 'decline' });
      row.remove();
    };
    row.appendChild(accept);
    row.appendChild(decline);
    transcript.appendChild(row);
    transcript.scrollTop = transcript.scrollHeight;
  }

  // 实时 item 事件 → 流水(不重画整段)。
  function handleItemEvent(method, params) {
    if (!params || params.threadId !== currentThreadId) return;
    if (method === 'item/started') {
      const item = params.item || {};
      const kind = item.type === 'text' ? 'assistant'
        : item.type === 'thinking' ? 'tool'
          : item.type === 'tool' ? 'tool'
            : item.type === 'error' ? 'error' : 'tool';
      const label = item.type === 'tool' ? ('工具 ' + (item.name || item.toolName || '?'))
        : item.type === 'thinking' ? '思考'
          : item.type === 'error' ? '错误' : '助理';
      const textNode = appendBubble(kind, label, item.text || '');
      itemRows.set(item.id, { li: null, textNode: textNode });
    } else if (method === 'item/delta') {
      const row = itemRows.get(params.itemId);
      if (row && row.textNode) {
        row.textNode.textContent += params.delta || '';
        transcript.scrollTop = transcript.scrollHeight;
      }
    } else if (method === 'item/completed') {
      const item = params.item || {};
      const row = itemRows.get(item.id);
      if (row && row.textNode && item.text) {
        row.textNode.textContent = item.text; // completed 带全文,以它为准
      } else if (!row && item.text) {
        appendBubble(item.type === 'tool' ? 'tool' : 'assistant',
          item.type === 'tool' ? '工具 ' + (item.name || '?') : '助理', item.text);
      }
    }
  }

  function setTurnRunning(running, lastStatus) {
    turnRunning = running;
    sendButton.disabled = running;
    interruptButton.disabled = !running;
    turnStatusLabel.textContent = running ? '在跑…'
      : (lastStatus ? ('上次回合: ' + lastStatus) : '');
  }

  // thread/read 的历史条目 → 流水(恢复用;content 块: text/tool_use/tool_result)
  function renderHistoryItem(entry) {
    if (entry.kind === 'compact_marker') {
      appendBubble('tool', '压缩', '上下文已压缩(seq ' + entry.seq + ')');
      return;
    }
    for (const block of entry.content || []) {
      if (block.type === 'text' && block.text) {
        appendBubble(entry.role === 'user' ? 'user' : 'assistant',
          entry.role === 'user' ? '你' : '助理', block.text);
      } else if (block.type === 'tool_use') {
        appendBubble('tool', '工具 ' + (block.name || '?'), JSON.stringify(block.input || {}));
      } else if (block.type === 'tool_result') {
        appendBubble('tool', '工具结果' + (block.isError ? '(失败)' : ''), String(block.content || ''));
      }
    }
  }

  // ---- 历史侧栏(thread/list 投影;点击 = 换场 + thread/read 恢复) ----
  let knownThreads = [];
  function refreshThreadList() {
    if (!channel) return Promise.resolve();
    return channel.request('thread/list', { scope: 'cwd' }).then(function (reply) {
      knownThreads = (reply.result && reply.result.threads) || [];
      threadList.textContent = '';
      for (const entry of knownThreads.slice(0, 50)) {
        const li = document.createElement('li');
        li.textContent = entry.title || entry.firstUserText || String(entry.threadId).slice(0, 12);
        li.title = (entry.title || '') + ' · ' + (entry.updatedAt || '') +
          ' · ' + (entry.messageCount || 0) + ' 条';
        if (entry.threadId === currentThreadId) li.classList.add('active');
        li.addEventListener('click', function () { openThread(entry.threadId); });
        threadList.appendChild(li);
      }
    }).catch(function () { /* 列表失败不掀聊天 */ });
  }

  function openThread(threadId) {
    currentThreadId = threadId;
    sessionStorage.setItem('assistant.lastThread', threadId);
    threadIdLabel.textContent = '会话 ' + String(threadId).slice(0, 8);
    resetTranscript();
    setTurnRunning(false, '');
    // 领域快照恢复(§六:刷新恢复走 thread/read,不靠页面攒的账)。
    channel.request('thread/read', { threadId: threadId, lastSeq: 0 }).then(function (reply) {
      const result = reply.result || {};
      if (result.sourceFormat === 'v2') {
        appendBubble('tool', '旧账', '这场是旧格式账,只支持新会话');
        return;
      }
      for (const entry of result.items || []) {
        renderHistoryItem(entry);
      }
    }).catch(function (error) {
      appendBubble('error', '历史读取失败', String(error.message || error));
    });
  }

  // ---- 连接与认证 ----
  function setConn(text, on) {
    connState.textContent = text;
    connState.className = 'state ' + (on ? 'on' : 'off');
  }

  function showNotice(title, body, actionText, action) {
    noticeTitle.textContent = title;
    noticeBody.textContent = body;
    if (actionText) {
      noticeAction.textContent = actionText;
      noticeAction.hidden = false;
      noticeAction.onclick = function () {
        hideNotice();
        action();
      };
    } else {
      noticeAction.hidden = true;
    }
    noticeOverlay.hidden = false;
  }

  function hideNotice() {
    noticeOverlay.hidden = true;
  }

  function attachChannelHandlers() {
    channel.onEvent('item/started', function (params) { handleItemEvent('item/started', params); });
    channel.onEvent('item/delta', function (params) { handleItemEvent('item/delta', params); });
    channel.onEvent('item/completed', function (params) { handleItemEvent('item/completed', params); });
    channel.onEvent('turn/completed', function (params) {
      if (params.threadId !== currentThreadId) return;
      setTurnRunning(false, params.executionStatus || params.status || 'completed');
      refreshThreadList();
    });
    channel.onEvent('permission/request', function (params) {
      if (params.threadId && params.threadId !== currentThreadId) return;
      appendApprovalRow(params);
    });
  }

  function connectChannel() {
    channel = new core.ProtocolChannel({
      port: Number(window.location.port) || 80,
      path: takeoverArmed ? '/ws?takeover=1' : '/ws',
      onDisconnect: function () {
        setConn('已断线(任务仍在跑,重连后补账)', false);
        setTurnRunning(false, '');
        scheduleReconnect();
      },
      onOccupied: function () {
        showNotice('已有页面持有控制连接',
          '这个助理已有一页连着。接管会替换那页的控制连接,不影响在跑的任务。',
          '接管', function () {
            takeoverArmed = true;
            if (channel) channel.close();
            connectChannel();
          });
      },
    });
    attachChannelHandlers();
    channel.connect().then(function (init) {
      const compat = core.checkProtocolCompat(init);
      if (!compat.ok) {
        showNotice('协议不匹配', compat.reason);
        return;
      }
      takeoverArmed = false;
      setConn('已连接', true);
      channel.request('assistant/status', {}).then(function (reply) {
        const status = reply.result || {};
        assistantMeta.textContent = 'profile=' + (status.profile || '?') +
          ' · ' + (status.lubancodeVersion || '') + ' · detached';
        const info = el('assistant-info');
        info.textContent = '';
        const rows = [
          ['工作目录', status.cwd || ''], ['bootId', status.bootId || ''],
          ['断线合同', 'detached(关页不停任务)'],
        ];
        for (const pair of rows) {
          const dt = document.createElement('dt'); dt.textContent = pair[0];
          const dd = document.createElement('dd'); dd.textContent = pair[1];
          info.appendChild(dt); info.appendChild(dd);
        }
      }).catch(function () { /* 状态失败不掀 */ });
      refreshThreadList().then(function () {
        const last = sessionStorage.getItem('assistant.lastThread');
        const known = knownThreads.some(function (t) { return t.threadId === last; });
        if (last && known) {
          openThread(last);
        }
      });
      refreshConfigStatus();
    }).catch(function (error) {
      setConn('连不上', false);
      showNotice('连接失败',
        String(error.message || error) + '。若会话已过期,从 lubancode assistant 打印的新地址进入。');
    });
  }

  let reconnectTimer = null;
  function scheduleReconnect() {
    if (reconnectTimer) return;
    reconnectTimer = setTimeout(function () {
      reconnectTimer = null;
      connectChannel();
    }, 2000);
  }

  // ---- 模型配置(首配流程;保存与测试分开显示,§五) ----
  function refreshConfigStatus() {
    if (!channel) return;
    channel.request('config/status', {}).then(function (reply) {
      const status = reply.result || {};
      const where = status.apiKeySource === 'inline' ? '已存配置文件(不回显)'
        : (status.apiKeySource && status.apiKeySource.indexOf('env:') === 0
          ? ('环境变量 ' + status.apiKeySource.slice(4)) : '未配置');
      configStatusLine.textContent = status.configured
        ? ('已配置:' + (status.provider || '?') + ' · ' + (status.model || '?') +
          ' · ' + (status.baseUrl || '') + ' · 密钥:' + where)
        : '还没配模型——填下面的表单,保存后即可聊天。';
      if (status.baseUrl && !el('config-base-url').value) {
        el('config-base-url').value = status.baseUrl;
      }
      if (status.model && !el('config-model').value) {
        el('config-model').value = status.model;
      }
    }).catch(function () { /* 状态失败不掀 */ });
  }

  configForm.addEventListener('submit', function (event) {
    event.preventDefault();
    if (!channel) return;
    configSaveResult.textContent = '保存中…';
    channel.request('config/model/set', {
      wire: el('config-wire').value,
      baseUrl: el('config-base-url').value.trim(),
      apiKey: el('config-api-key').value,
      model: el('config-model').value.trim(),
    }).then(function (reply) {
      if (reply.error) {
        configSaveResult.textContent = '保存失败:' + (reply.error.message || '未知错误');
        return;
      }
      configSaveResult.textContent = '已保存(' + ((reply.result && reply.result.model) || '') + ')';
      el('config-api-key').value = ''; // 提交即忘,不留页面
      refreshConfigStatus();
    }).catch(function (error) {
      configSaveResult.textContent = '保存出错:' + String(error.message || error);
    });
  });

  el('config-test-button').addEventListener('click', function () {
    if (!channel) return;
    configTestResult.textContent = '测试中…';
    channel.request('config/test', {}).then(function (reply) {
      const result = reply.result || {};
      configTestResult.textContent = (result.reachable ? '端点可达' : '连不上') +
        (result.detail ? ':' + result.detail : '');
    }).catch(function (error) {
      configTestResult.textContent = '测试出错:' + String(error.message || error);
    });
  });

  stopAssistantButton.addEventListener('click', function () {
    if (!channel) return;
    showNotice('停止助理?',
      '会停掉整个助理进程;在跑的任务按打断收口。只停当前任务用聊天页的"停止任务"。',
      '确认停止', function () {
        channel.request('shutdown', {}).then(function () {
          setConn('助理已停止', false);
        }).catch(function () { /* 进程退出会断线,走 onDisconnect */ });
      });
  });

  // ---- 聊天 ----
  newThreadButton.addEventListener('click', function () {
    if (!channel) return;
    const opId = (window.crypto && crypto.randomUUID) ? crypto.randomUUID() : String(Date.now());
    channel.request('thread/start', { clientOperationId: opId }).then(function (reply) {
      if (reply.error) {
        appendBubble('error', '开会话失败', reply.error.message || '');
        return;
      }
      const result = reply.result || {};
      if (result.duplicate && result.threadId) {
        openThread(result.threadId);
        return;
      }
      currentThreadId = result.threadId || '';
      sessionStorage.setItem('assistant.lastThread', currentThreadId);
      threadIdLabel.textContent = '会话 ' + String(currentThreadId).slice(0, 8);
      resetTranscript();
      refreshThreadList();
    }).catch(function (error) {
      appendBubble('error', '开会话出错', String(error.message || error));
    });
  });

  composer.addEventListener('submit', function (event) {
    event.preventDefault();
    const text = sayInput.value.trim();
    if (!text || !channel) return;
    if (!currentThreadId) {
      appendBubble('error', '提示', '先点"新会话"开一场,再说话。');
      return;
    }
    sayInput.value = '';
    appendBubble('user', '你', text);
    const opId = (window.crypto && crypto.randomUUID) ? crypto.randomUUID() : String(Date.now());
    setTurnRunning(true, '');
    channel.request('turn/start', {
      threadId: currentThreadId,
      text: text,
      clientOperationId: opId,
    }).then(function (reply) {
      if (reply.error) {
        setTurnRunning(false, '');
        appendBubble('error', '发送失败', reply.error.message || '');
      }
    }).catch(function (error) {
      setTurnRunning(false, '');
      appendBubble('error', '发送出错', String(error.message || error));
    });
  });

  interruptButton.addEventListener('click', function () {
    if (!channel || !currentThreadId) return;
    channel.request('turn/interrupt', { threadId: currentThreadId }).catch(function () { /* 断线时按钮自然失效 */ });
  });

  sayInput.addEventListener('keydown', function (event) {
    if (event.key === 'Enter' && !event.shiftKey) {
      event.preventDefault();
      composer.requestSubmit();
    }
  });

  // ---- 视图切换 ----
  const views = { chat: el('chat-view'), config: el('config-view'), tasks: el('tasks-view') };
  document.querySelectorAll('#topnav button[data-view]').forEach(function (button) {
    button.addEventListener('click', function () {
      const name = button.dataset.view;
      for (const key of Object.keys(views)) {
        views[key].hidden = key !== name;
      }
      document.querySelectorAll('#topnav button').forEach(function (b) {
        b.classList.toggle('active', b === button);
        if (b === button) { b.setAttribute('aria-current', 'page'); } else { b.removeAttribute('aria-current'); }
      });
      if (name === 'config') refreshConfigStatus();
    });
  });

  // ---- 启动:fragment 换 cookie → 连 WS ----
  window.addEventListener('DOMContentLoaded', function () {
    const fragment = core.readBootstrapFragment();
    const boot = fragment
      ? core.exchangeBootstrap(fragment).then(function (result) {
        if (result.ok) {
          core.clearBootstrapFragment();
          return true;
        }
        showNotice('进入链接已失效', result.reason);
        return false;
      })
      : Promise.resolve(true); // 无 fragment:可能带着上次的 cookie,直接连。
    boot.then(function (authed) {
      if (!authed) return;
      connectChannel();
    });
  });
})();
