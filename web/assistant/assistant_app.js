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
  // W2:任务/结果/审批
  const taskForm = el('task-form');
  const taskPrompt = el('task-prompt');
  const taskCreateResult = el('task-create-result');
  const taskFaceHint = el('task-face-hint');
  const taskListEl = el('task-list');
  const taskEmptyHint = el('task-empty-hint');
  const resultListEl = el('result-list');
  const resultEmptyHint = el('result-empty-hint');
  const approvalListEl = el('approval-list');
  const approvalEmptyHint = el('approval-empty-hint');
  const approvalBadge = el('approval-badge');

  let channel = null;
  let takeoverArmed = false; // 显式接管旗(§六:接管只换控制连接)
  let occupiedAutoRetried = false; // 刷新竞态的自救一次:服务端读到旧连接
                                   // EOF 有窗口,先退避重连一次再谈接管
  let currentThreadId = '';
  let turnRunning = false;
  let answeredApprovals = new Set();

  // ---- W2:任务/审批的页面投影(真账在服务端;这里只是缓存,事件/刷新
  // 双驱动)。补账游标:断线重连(同页面实例)增量续;刷新(新实例)从
  // 0 全量。bootId 绑定:服务端重启后 reset,快照重读。 ----
  let taskSnapshot = { tasks: [], automationAvailable: true };
  let pendingApprovals = [];
  let resolvedApprovals = [];           // 最近已决(页面内的审计行,刷新即清)
  let lastEventBootId = '';
  let lastEventSeq = 0;
  let taskRefreshTimer = null;

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
    // W2:任务/审批事件(实时推送;错过走 assistant/events/read 补账)。
    // params.seq 是服务端事件账的游标——实时见过即推进,重连补账不重发。
    channel.onEvent('assistant/task/event', function (params) {
      noteEventSeq(params);
      scheduleTaskRefresh();
    });
    channel.onEvent('assistant/approval/request', function (params) {
      noteEventSeq(params);
      upsertPendingApproval(params);
    });
    channel.onEvent('assistant/approval/resolved', function (params) {
      noteEventSeq(params);
      resolvePendingApproval(params);
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
        // 刷新/断线立刻重连会撞上"服务端还没读到旧连接 EOF"的窗口——
        // 先自救重试一次;仍占用才是真有别的页面连着,给接管对话。
        if (!occupiedAutoRetried) {
          occupiedAutoRetried = true;
          setTimeout(function () { connectChannel(); }, 400);
          return;
        }
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
      occupiedAutoRetried = false;
      setConn('已连接', true);
      channel.request('assistant/status', {}).then(function (reply) {
        const status = reply.result || {};
        assistantMeta.textContent = 'profile=' + (status.profile || '?') +
          ' · ' + (status.lubancodeVersion || '') + ' · detached';
        const info = el('assistant-info');
        info.textContent = '';
        const automation = status.automation || {};
        const automationLine = automation.available
          ? '可用(单次任务/审批/结果)'
          : ('不可用:' + (automation.reason || '未知原因'));
        const rows = [
          ['工作目录', status.cwd || ''], ['bootId', status.bootId || ''],
          ['断线合同', 'detached(关页不停任务)'],
          ['任务面', automationLine],
        ];
        for (const pair of rows) {
          const dt = document.createElement('dt'); dt.textContent = pair[0];
          const dd = document.createElement('dd'); dd.textContent = pair[1];
          info.appendChild(dt); info.appendChild(dd);
        }
        taskSnapshot.automationAvailable = automation.available !== false;
        taskFaceHint.hidden = automation.available !== false;
        if (!taskSnapshot.automationAvailable) {
          taskFaceHint.textContent = '任务面不可用:' + (automation.reason || '未知原因') +
            '。聊天不受影响。';
          taskForm.querySelector('button[type=submit]').disabled = true;
        }
        // 重连补账(§六 W2):按 (bootId, lastSeq) 补齐断线期间错过的
        // 任务/审批事件;bootId 不符/缺口被挤出 → 服务端 reset=true,
        // 页面清本地投影快照重读(页面不是账本)。
        catchUpMissedEvents(status.bootId || '');
      }).catch(function () { /* 状态失败不掀 */ });
      refreshThreadList().then(function () {
        const last = sessionStorage.getItem('assistant.lastThread');
        const known = knownThreads.some(function (t) { return t.threadId === last; });
        if (last && known) {
          openThread(last);
        }
      });
      refreshConfigStatus();
      refreshTasks();
      refreshApprovals();
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

  // ---- W2:任务 / 结果 / 审批 / 补账 ----

  function newOperationId() {
    return (window.crypto && crypto.randomUUID) ? crypto.randomUUID() : String(Date.now());
  }

  // 实时事件带的服务端游标:见过即推进(重连补账从这之后续,不重发)。
  function noteEventSeq(params) {
    if (params && Number.isFinite(params.seq) && params.seq > lastEventSeq) {
      lastEventSeq = params.seq;
      lastEventBootId = lastEventBootId || '';
    }
  }

  // 断线补账:按 (bootId, lastSeq) 读事件账。reset(服务端重启/缺口挤出)
  // → 清本地投影,快照重读;正常 → 增量应用,游标前进。
  function catchUpMissedEvents(currentBootId) {
    if (!channel) return;
    const bootId = lastEventBootId || currentBootId;
    channel.request('assistant/events/read', { bootId: bootId, lastSeq: lastEventSeq })
      .then(function (reply) {
        const result = reply.result;
        if (!result || reply.error) return;
        lastEventBootId = result.bootId || currentBootId;
        if (result.reset) {
          pendingApprovals = [];
          resolvedApprovals = [];
        }
        for (const entry of result.events || []) {
          applyAssistantEvent(entry.method, entry.params || {});
          if (entry.seq > lastEventSeq) {
            lastEventSeq = entry.seq;
          }
        }
        if (result.reset) {
          refreshTasks();
          refreshApprovals();
        }
        renderApprovals();
      }).catch(function () { /* 补账失败不掀;快照刷新兜底 */ });
  }

  function applyAssistantEvent(method, params) {
    if (method === 'assistant/task/event') {
      scheduleTaskRefresh();
    } else if (method === 'assistant/approval/request') {
      upsertPendingApproval(params);
    } else if (method === 'assistant/approval/resolved') {
      resolvePendingApproval(params);
    }
  }

  // 任务列表刷新的节流(事件风暴不逐枚重画)。
  function scheduleTaskRefresh() {
    if (taskRefreshTimer) return;
    taskRefreshTimer = setTimeout(function () {
      taskRefreshTimer = null;
      refreshTasks();
    }, 400);
  }

  function refreshTasks() {
    if (!channel) return Promise.resolve();
    return channel.request('task/list', {}).then(function (reply) {
      if (reply.error) return;
      taskSnapshot.tasks = (reply.result && reply.result.tasks) || [];
      renderTasks();
      renderResults();
    }).catch(function () { /* 列表失败不掀 */ });
  }

  function refreshApprovals() {
    if (!channel) return Promise.resolve();
    return channel.request('approval/list', {}).then(function (reply) {
      if (reply.error) return;
      pendingApprovals = (reply.result && reply.result.pending) || [];
      renderApprovals();
    }).catch(function () { /* 失败不掀 */ });
  }

  const outcomeLabel = {
    succeeded: '成功', failed: '失败', needs_review: '待核对', cancelled: '已取消',
  };
  const stateLabel = { scheduled: '排队', claimed: '执行中', settled: '已结算' };
  const deliveryLabel = { delivered: '已投递', pending: '待投递', flagged: '投递异常' };

  function chip(text, cls) {
    const span = document.createElement('span');
    span.className = 'state-chip' + (cls ? ' ' + cls : '');
    span.textContent = text;
    return span;
  }

  function renderTasks() {
    taskListEl.textContent = '';
    const tasks = taskSnapshot.tasks;
    taskEmptyHint.hidden = tasks.length > 0;
    for (const task of tasks) {
      const card = document.createElement('li');
      card.className = 'task-card';
      const head = document.createElement('div');
      head.className = 'head';
      const prompt = document.createElement('span');
      prompt.className = 'prompt';
      prompt.textContent = task.prompt || task.jobId;
      head.appendChild(prompt);
      const latest = task.latest || {};
      if (latest.state) {
        head.appendChild(chip(stateLabel[latest.state] || latest.state, latest.state === 'claimed' ? 'ok' : ''));
      }
      if (latest.outcome) {
        head.appendChild(chip(outcomeLabel[latest.outcome] || latest.outcome,
          latest.outcome === 'succeeded' ? 'ok' : (latest.outcome === 'needs_review' || latest.outcome === 'failed' ? 'warn' : '')));
      }
      if (task.state && task.state !== 'active') {
        head.appendChild(chip(task.state === 'cancelled' ? '任务已取消' : task.state, 'warn'));
      }
      card.appendChild(head);

      const ops = document.createElement('div');
      ops.className = 'ops';
      const detailButton = document.createElement('button');
      detailButton.type = 'button';
      detailButton.textContent = '详情/结果';
      const detailHost = document.createElement('div');
      detailHost.className = 'detail';
      detailHost.hidden = true;
      detailButton.addEventListener('click', function () {
        detailHost.hidden = !detailHost.hidden;
        if (!detailHost.hidden && detailHost.textContent === '') {
          fillTaskDetail(task.jobId, detailHost);
        }
      });
      ops.appendChild(detailButton);
      if (task.state === 'active' && (!latest.state || latest.state !== 'settled')) {
        const cancelButton = document.createElement('button');
        cancelButton.type = 'button';
        cancelButton.textContent = '取消任务';
        cancelButton.addEventListener('click', function () {
          cancelTask(task.jobId, task.revision, cancelButton);
        });
        ops.appendChild(cancelButton);
      }
      card.appendChild(ops);
      card.appendChild(detailHost);
      taskListEl.appendChild(card);
    }
  }

  function fillTaskDetail(jobId, host) {
    if (!channel) return;
    host.textContent = '读取中…';
    channel.request('task/read', { jobId: jobId }).then(function (reply) {
      host.textContent = '';
      if (reply.error) {
        host.textContent = '读取失败:' + (reply.error.message || '');
        return;
      }
      const occurrences = (reply.result && reply.result.occurrences) || [];
      if (occurrences.length === 0) {
        host.textContent = '还没有执行记录。';
        return;
      }
      for (const occurrence of occurrences) {
        const line = document.createElement('div');
        line.textContent = (stateLabel[occurrence.state] || occurrence.state) +
          (occurrence.outcome ? ' · ' + (outcomeLabel[occurrence.outcome] || occurrence.outcome) : '') +
          (occurrence.detail ? ' · ' + occurrence.detail : '');
        host.appendChild(line);
        const result = occurrence.result;
        if (result) {
          const delivery = document.createElement('div');
          delivery.textContent = '结果:' + (deliveryLabel[result.deliveryState] || result.deliveryState) +
            (result.publishedPath ? '(' + result.publishedPath + ')' : '');
          host.appendChild(delivery);
          if (result.replyText) {
            const body = document.createElement('div');
            body.textContent = result.replyText;
            host.appendChild(body);
          }
        }
      }
    }).catch(function (error) {
      host.textContent = '读取出错:' + String(error.message || error);
    });
  }

  function cancelTask(jobId, revision, button) {
    if (!channel) return;
    button.disabled = true;
    channel.request('task/cancel', {
      jobId: jobId, expectedRevision: revision, clientOperationId: newOperationId(),
    }).then(function (reply) {
      if (reply.error) {
        button.disabled = false;
        button.textContent = '取消失败(核对后重试)';
        return;
      }
      refreshTasks();
    }).catch(function () { button.disabled = false; });
  }

  function renderResults() {
    resultListEl.textContent = '';
    let shown = 0;
    for (const task of taskSnapshot.tasks) {
      const latest = task.latest;
      if (!latest || latest.state !== 'settled' || !latest.result) continue;
      ++shown;
      const card = document.createElement('li');
      card.className = 'result-card';
      const head = document.createElement('div');
      head.className = 'head';
      const prompt = document.createElement('span');
      prompt.className = 'prompt';
      prompt.textContent = task.prompt || task.jobId;
      head.appendChild(prompt);
      const result = latest.result || {};
      const state = result.deliveryState || '';
      head.appendChild(chip(outcomeLabel[latest.outcome] || latest.outcome || '',
        latest.outcome === 'succeeded' ? 'ok' : 'warn'));
      head.appendChild(chip(deliveryLabel[state] || state,
        state === 'delivered' ? 'ok' : (state === 'flagged' ? 'warn' : '')));
      card.appendChild(head);
      if (result.replyText) {
        const body = document.createElement('div');
        body.className = 'detail';
        body.textContent = result.replyText;
        card.appendChild(body);
      }
      resultListEl.appendChild(card);
    }
    resultEmptyHint.hidden = shown > 0;
  }

  function upsertPendingApproval(params) {
    if (!params || !params.requestId) return;
    const known = pendingApprovals.some(function (item) { return item.requestId === params.requestId; });
    if (!known) {
      pendingApprovals.push(params);
    }
    renderApprovals();
  }

  function resolvePendingApproval(params) {
    if (!params || !params.requestId) return;
    pendingApprovals = pendingApprovals.filter(function (item) { return item.requestId !== params.requestId; });
    resolvedApprovals.unshift(params);
    if (resolvedApprovals.length > 20) {
      resolvedApprovals.length = 20;
    }
    renderApprovals();
    scheduleTaskRefresh();  // 审批落定,任务要继续跑
  }

  function renderApprovals() {
    approvalListEl.textContent = '';
    const pending = pendingApprovals;
    approvalEmptyHint.hidden = pending.length > 0;
    approvalBadge.hidden = pending.length === 0;
    approvalBadge.textContent = pending.length > 0 ? String(pending.length) : '';
    for (const item of pending) {
      approvalListEl.appendChild(buildApprovalCard(item, true));
    }
    for (const item of resolvedApprovals) {
      approvalListEl.appendChild(buildApprovalCard(item, false));
    }
  }

  function buildApprovalCard(item, actionable) {
    const card = document.createElement('li');
    card.className = 'approval-card' + (actionable ? '' : ' resolved');
    const head = document.createElement('div');
    head.className = 'head';
    const tool = document.createElement('span');
    tool.className = 'tool';
    tool.textContent = item.toolName || '工具';
    head.appendChild(tool);
    if (actionable) {
      head.appendChild(chip('等审批', 'warn'));
    } else {
      const outcomeText = item.outcome === 'accepted' ? '已批准'
        : item.outcome === 'declined' ? '已拒绝'
          : item.outcome === 'timeout_declined' ? '超时拒绝(未执行)' : (item.outcome || '已收口');
      const outcome = document.createElement('span');
      outcome.className = 'outcome' + (item.outcome === 'timeout_declined' ? ' timeout' : '');
      outcome.textContent = outcomeText;
      head.appendChild(outcome);
    }
    card.appendChild(head);
    if (item.inputPreview) {
      const preview = document.createElement('div');
      preview.className = 'input-preview';
      preview.textContent = item.inputPreview;
      card.appendChild(preview);
    }
    if (item.jobId) {
      const job = document.createElement('div');
      job.className = 'hint';
      job.textContent = '任务:' + String(item.jobId).slice(0, 24) + (item.occurrenceId ? ' · 执行 ' + String(item.occurrenceId).slice(0, 20) : '');
      card.appendChild(job);
    }
    if (actionable) {
      const ops = document.createElement('div');
      ops.className = 'ops';
      const accept = document.createElement('button');
      accept.type = 'button';
      accept.textContent = '批准';
      accept.addEventListener('click', function () {
        respondApproval(item.requestId, 'accept', accept);
      });
      const decline = document.createElement('button');
      decline.type = 'button';
      decline.textContent = '拒绝';
      decline.addEventListener('click', function () {
        respondApproval(item.requestId, 'decline', decline);
      });
      ops.appendChild(accept);
      ops.appendChild(decline);
      card.appendChild(ops);
    }
    return card;
  }

  function respondApproval(requestId, decision, button) {
    if (!channel) return;
    button.disabled = true;
    channel.request('approval/respond', { requestId: requestId, decision: decision })
      .then(function (reply) {
        if (reply.error || !reply.result || reply.result.resolved !== true) {
          button.disabled = false;
          button.textContent = '答复迟到(已失效)';
          refreshApprovals();
          return;
        }
        refreshApprovals();
      }).catch(function () { button.disabled = false; });
  }

  taskForm.addEventListener('submit', function (event) {
    event.preventDefault();
    if (!channel) return;
    const prompt = taskPrompt.value.trim();
    if (!prompt) return;
    if (!taskSnapshot.automationAvailable) {
      taskCreateResult.textContent = '任务面不可用(见设置页运行信息)';
      return;
    }
    taskCreateResult.textContent = '提交中…';
    const opId = newOperationId();
    channel.request('task/create', { prompt: prompt, clientOperationId: opId }).then(function (reply) {
      if (reply.error) {
        taskCreateResult.textContent = '创建失败:' + (reply.error.message || '');
        return;
      }
      const result = reply.result || {};
      taskCreateResult.textContent = result.duplicate ? '已受理(重复提交回原任务)' : '已受理,执行中';
      taskPrompt.value = '';
      refreshTasks();
    }).catch(function (error) {
      // 网络不明:幂等键在,原键重试不双建(§六)。
      taskCreateResult.textContent = '提交出错(' + String(error.message || error) + ');重试会复用同一操作号,不会建两个任务';
      sessionStorage.setItem('assistant.retryTaskOp', opId);
      sessionStorage.setItem('assistant.retryTaskPrompt', prompt);
    });
  });

  el('task-refresh-button').addEventListener('click', function () { refreshTasks(); });
  el('result-refresh-button').addEventListener('click', function () { refreshTasks(); });

  // ---- 视图切换 ----
  const views = { chat: el('chat-view'), config: el('config-view'), tasks: el('tasks-view'),
    results: el('results-view'), approvals: el('approvals-view') };
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
      if (name === 'tasks' || name === 'results') refreshTasks();
      if (name === 'approvals') refreshApprovals();
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
