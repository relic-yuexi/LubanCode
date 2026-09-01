// LubanCode 参考前端——渲染层(多前端外壳单阶段 D)。
//
// 只做两件事:把 ConsoleState 的账画到 DOM 上;把用户的动作折成协议请求。
// 协议路全在 web_console_core.js(与 Node 冒烟同一份);这层不许自己拼
// 协议消息,不许读内核盘上账。换掉这层(换个框架重写)不动内核一行——
// 这就是本页存在的意义(单子阶段 D 验收)。
'use strict';

(function () {
  const core = window.LubanWebConsole;

  const el = (id) => document.getElementById(id);
  const els = {
    connectForm: el('connect-form'), portInput: el('port-input'), tokenInput: el('token-input'),
    connectButton: el('connect-button'), disconnectButton: el('disconnect-button'),
    connState: el('conn-state'), base64State: el('base64-state'),
    newThreadButton: el('new-thread-button'), threadIdLabel: el('thread-id-label'),
    turnStatusLabel: el('turn-status-label'), usageLabel: el('usage-label'),
    transcript: el('transcript'), composer: el('composer'), sayInput: el('say-input'),
    interruptButton: el('interrupt-button'),
    browserStart: el('browser-start-button'), browserStop: el('browser-stop-button'),
    browserPause: el('browser-pause-button'), browserResume: el('browser-resume-button'),
    browserStateLabel: el('browser-state-label'),
    pageList: el('page-list'), navigateForm: el('navigate-form'), urlInput: el('url-input'),
    openNewButton: el('open-new-button'),
    consoleView: el('console-view'), networkView: el('network-view'), downloadsView: el('downloads-view'),
    screencastStart: el('screencast-start-button'), screencastStop: el('screencast-stop-button'),
    snapshotButton: el('snapshot-button'), screenshotButton: el('screenshot-button'),
    mirrorStateLabel: el('mirror-state-label'),
    mirrorImage: el('mirror-image'), screenshotImage: el('screenshot-image'),
    elementList: el('element-list'), typeForm: el('type-form'), typeInput: el('type-input'),
    eventLog: el('event-log'),
    approvalOverlay: el('approval-overlay'), approvalList: el('approval-list'),
  };

  const session = {
    channel: null,
    state: null,
    port: 0,
    token: '',
    compat: null,
    activePageId: '',
    selectedRef: '',
    lastSnapshotId: '',
    lastSnapshotRefs: [],
    userMessageSeq: 0,
  };

  // -----------------------------------------------------------------------
  // 小工具
  // -----------------------------------------------------------------------

  function text(node, value) {
    node.textContent = value;
    return node;
  }

  function clear(node) {
    while (node.firstChild) {
      node.removeChild(node.firstChild);
    }
  }

  function activePage(state) {
    const pages = state.browser.pages;
    if (session.activePageId && pages[session.activePageId]) {
      return session.activePageId;
    }
    for (const id of Object.keys(pages)) {
      if (pages[id].active) {
        return id;
      }
    }
    return Object.keys(pages)[0] || '';
  }

  function note(label, message) {
    console.log('[web-console] ' + label + (message ? ': ' + message : ''));
  }

  function fail(label, error) {
    console.error('[web-console] ' + label + ':', error);
    window.alert(label + ' 失败: ' + (error && error.message ? error.message : error));
  }

  // -----------------------------------------------------------------------
  // 渲染(账 → DOM)
  // -----------------------------------------------------------------------

  function renderConnection() {
    const on = session.channel !== null && !session.channel.closed;
    // 兼容检测提示(打包发布账 §四):连得上但协议不合,状态条转 warn 摆
    // 人话——提示,不拦截;合的就只报版本。
    if (on && session.compat && !session.compat.ok) {
      els.connState.textContent = '已连(协议不合):' + session.compat.hint;
      els.connState.className = 'state warn';
      return;
    }
    els.connState.textContent = on ? '已连接 :' + session.port + (session.compat ? '(协议 ' + session.compat.server + ')' : '') : '未连接';
    els.connState.className = 'state ' + (on ? 'on' : 'off');
    els.connectButton.hidden = on;
    els.disconnectButton.hidden = !on;
    els.base64State.hidden = !session.channel || !session.channel.sawBase64;
  }

  function renderChat(state) {
    els.threadIdLabel.textContent = state.currentThreadId
      ? '会话 ' + state.currentThreadId : '(还没开会话)';
    const turn = state.turnStatusByThread[state.currentThreadId];
    els.turnStatusLabel.textContent = turn ? ('回合 ' + turn.status) : '';
    const usage = state.usageTotal;
    els.usageLabel.textContent = 'tokens 入' + usage.inputTokens + '/出' + usage.outputTokens +
      '/缓读' + usage.cacheReadTokens;
    clear(els.transcript);
    for (const item of state.transcript()) {
      const li = document.createElement('li');
      li.className = item.type === 'thinking' ? 'thinking'
        : item.type === 'user' ? 'user'
          : item.type === 'error' ? 'error-item'
            : item.type === 'tool' || item.type === 'command' ? 'tool' : '';
      const head = item.tool ? '[' + item.tool + '] ' : item.type === 'thinking' ? '(思考) ' : '';
      let body = item.text || '';
      if (item.type === 'tool' || item.type === 'command') {
        if (item.input) {
          body += '\n入参 ' + JSON.stringify(item.input);
        }
        if (item.result !== undefined && item.result !== '') {
          body += '\n结果 ' + String(item.result);
        }
        if (item.images && item.images.length) {
          for (const image of item.images) {
            const img = document.createElement('img');
            img.src = core.buildArtifactUrl(session.port, image.artifact, session.token);
            img.style.maxWidth = '120px';
            li.appendChild(img);
          }
        }
        if (item.isError) {
          body += '\n(工具报错)';
        }
      }
      text(li, head + body);
      li.style.whiteSpace = 'pre-wrap';
      els.transcript.appendChild(li);
    }
    els.transcript.scrollTop = els.transcript.scrollHeight;
  }

  function renderBrowser(state) {
    const browser = state.browser;
    els.browserStateLabel.textContent = browser.crashedReason
      ? '已崩溃:' + browser.crashedReason
      : browser.launched
        ? (browser.session ? '在跑(' + (browser.session.engine || '?') + (browser.paused ? ',已暂停)' : ')') : '在跑')
        : '未起场';
    els.browserPause.disabled = browser.paused;
    els.browserResume.disabled = !browser.paused;

    clear(els.pageList);
    const pageId = activePage(state);
    for (const id of Object.keys(browser.pages)) {
      const page = browser.pages[id];
      const li = document.createElement('li');
      if (id === pageId) {
        li.className = 'active';
      }
      const select = document.createElement('button');
      text(select, id === pageId ? '当前' : '切换');
      select.onclick = () => {
        session.activePageId = id;
        session.channel.request('browser/page/select', { pageId: id }).catch((e) => fail('切页', e));
      };
      const title = document.createElement('span');
      title.className = 'page-title';
      text(title, id + ' ' + (page.title || '') + ' ' + (page.url || '') + ' g' + page.generation);
      const epoch = document.createElement('span');
      epoch.className = 'epoch';
      text(epoch, 'userEpoch=' + (browser.userEpoch[id] || 0));
      const close = document.createElement('button');
      text(close, '关页');
      close.onclick = () => {
        session.channel.request('browser/page/close', { pageId: id }).catch((e) => fail('关页', e));
      };
      li.appendChild(select);
      li.appendChild(title);
      li.appendChild(epoch);
      li.appendChild(close);
      els.pageList.appendChild(li);
    }
  }

  function consoleRow(entry) {
    const tr = document.createElement('tr');
    const cells = [
      String(entry.seq),
      entry.level || '',
      (entry.text || '') + (entry.sourceUrl ? ' @' + entry.sourceUrl + ':' + (entry.line || 0) : ''),
    ];
    for (let i = 0; i < cells.length; ++i) {
      const td = document.createElement('td');
      if (i === 2) {
        td.className = 'wide';
      }
      if (i === 1) {
        td.className = 'lvl-' + String(entry.level || '').toLowerCase();
      }
      text(td, cells[i]);
      tr.appendChild(td);
    }
    return tr;
  }

  function networkRow(entry) {
    const tr = document.createElement('tr');
    const cells = [
      String(entry.seq),
      entry.method || '',
      entry.status !== undefined ? String(entry.status) : (entry.failed ? '失败' : ''),
      entry.url || '',
    ];
    for (let i = 0; i < cells.length; ++i) {
      const td = document.createElement('td');
      if (i === 3) {
        td.className = 'wide';
      }
      if (i === 2 && entry.failed) {
        td.className = 'failed';
      }
      text(td, cells[i]);
      tr.appendChild(td);
    }
    return tr;
  }

  function renderJournals(state) {
    const pageId = activePage(state);
    const table = (header, rowFn) => {
      const t = document.createElement('table');
      const head = document.createElement('tr');
      for (const name of header) {
        const th = document.createElement('th');
        text(th, name);
        head.appendChild(th);
      }
      t.appendChild(head);
      return { table: t, rowFn: rowFn };
    };
    const consoleJournal = state.browser.console[pageId];
    const consoleTable = table(['seq', '级', '正文'], consoleRow);
    if (consoleJournal) {
      for (const entry of consoleJournal.rows) {
        consoleTable.table.appendChild(consoleRow(entry));
      }
    }
    clear(els.consoleView);
    if (consoleJournal && consoleJournal.dropped > 0) {
      const dropped = document.createElement('p');
      dropped.className = 'dropped-note';
      text(dropped, '丢了 ' + consoleJournal.dropped + ' 条(有界规矩;query 补账可见全量,lastSeq=' + consoleJournal.lastSeq + ')');
      els.consoleView.appendChild(dropped);
    }
    els.consoleView.appendChild(consoleTable.table);

    const networkJournal = state.browser.network[pageId];
    const networkTable = table(['seq', '法子', '状态', 'URL'], networkRow);
    if (networkJournal) {
      for (const entry of networkJournal.rows) {
        networkTable.table.appendChild(networkRow(entry));
      }
    }
    clear(els.networkView);
    if (networkJournal && networkJournal.dropped > 0) {
      const dropped = document.createElement('p');
      dropped.className = 'dropped-note';
      text(dropped, '丢了 ' + networkJournal.dropped + ' 条(元数据账,lastSeq=' + networkJournal.lastSeq + ')');
      els.networkView.appendChild(dropped);
    }
    els.networkView.appendChild(networkTable.table);

    clear(els.downloadsView);
    for (const download of state.browser.downloads) {
      const p = document.createElement('p');
      text(p, (download.state || '') + ' ' + (download.filename || download.suggested || '') +
        (download.bytes ? ' ' + download.bytes + 'B' : ''));
      els.downloadsView.appendChild(p);
    }
  }

  function renderMirror(state) {
    const pageId = activePage(state);
    const mirror = state.browser.mirror[pageId];
    if (mirror && mirror.artifact) {
      els.mirrorImage.hidden = false;
      const url = core.buildArtifactUrl(session.port, mirror.artifact, session.token);
      if (!els.mirrorImage.src.endsWith(url)) {
        els.mirrorImage.src = url;
      }
      els.mirrorStateLabel.textContent =
        '帧#' + mirror.frameSeq + (mirror.dropped ? '(丢' + mirror.dropped + ')' : '') +
        ' ' + mirror.width + 'x' + mirror.height;
    } else {
      els.mirrorImage.hidden = true;
      els.mirrorStateLabel.textContent = pageId ? '没在镜像' : '(没开页)';
    }
    const shot = state.browser.screenshot[pageId];
    if (shot && shot.artifact) {
      els.screenshotImage.hidden = false;
      els.screenshotImage.src = core.buildArtifactUrl(session.port, shot.artifact, session.token);
    } else {
      els.screenshotImage.hidden = true;
    }

    clear(els.elementList);
    for (const row of session.lastSnapshotRefs) {
      const li = document.createElement('li');
      if (row.ref === session.selectedRef) {
        li.className = 'selected';
      }
      li.dataset.ref = row.ref; // 触屏外套(阶段 E)按 DOM 命中点取 ref 用
      const ref = document.createElement('span');
      ref.className = 'ref';
      text(ref, row.ref);
      li.appendChild(ref);
      li.appendChild(document.createTextNode(row.text));
      li.onclick = () => {
        session.selectedRef = row.ref;
        els.typeForm.hidden = false;
        renderAll();
      };
      li.ondblclick = () => {
        injectClick(row.ref);
      };
      els.elementList.appendChild(li);
    }
  }

  function renderApprovals(state) {
    clear(els.approvalList);
    for (const approval of state.approvals) {
      const li = document.createElement('li');
      const name = document.createElement('div');
      name.className = 'tool-name';
      text(name, (approval.tool || '?') + '(thread ' + (approval.threadId || '?') + ')');
      const input = document.createElement('div');
      text(input, JSON.stringify(approval.input || {}));
      li.appendChild(name);
      li.appendChild(input);
      const actions = document.createElement('div');
      actions.className = 'actions';
      for (const decision of ['accept', 'acceptForSession', 'decline', 'cancel']) {
        const button = document.createElement('button');
        text(button, decision);
        button.onclick = () => {
          session.channel.answerServerRequest(approval.requestId, { decision: decision });
          const at = state.approvals.indexOf(approval);
          if (at >= 0) {
            state.approvals.splice(at, 1);
          }
          renderAll();
        };
        actions.appendChild(button);
      }
      li.appendChild(actions);
      els.approvalList.appendChild(li);
    }
    els.approvalOverlay.hidden = state.approvals.length === 0;
  }

  function renderAll() {
    if (!session.state) {
      return;
    }
    renderConnection();
    renderChat(session.state);
    renderBrowser(session.state);
    renderJournals(session.state);
    renderMirror(session.state);
    renderApprovals(session.state);
  }

  // -----------------------------------------------------------------------
  // 动作(DOM → 协议)
  // -----------------------------------------------------------------------

  function injectClick(ref) {
    const params = { kind: 'click', ref: ref, pageId: activePage(session.state) };
    if (session.lastSnapshotId) {
      params.snapshotId = session.lastSnapshotId;
    }
    session.channel.request('browser/action', params)
      .then(() => note('点击已受理', ref))
      .catch((e) => fail('点击', e));
  }

  function connect(port, token) {
    const channel = new core.ProtocolChannel({
      port: port,
      token: token,
      name: 'web-console',
      onDisconnect: () => {
        note('连接断了(cursor 补账的口径:重连后用 query 拉)');
        renderConnection();
      },
    });
    const state = new core.ConsoleState();
    state.attach(channel);
    state.onChange(renderAll);
    channel.onEvent('*', (params, message) => {
      const li = document.createElement('li');
      const m = document.createElement('span');
      m.className = 'm';
      text(m, message.method);
      li.appendChild(m);
      li.appendChild(document.createTextNode(' seq=' + ((params && params.seq) || '-')));
      els.eventLog.insertBefore(li, els.eventLog.firstChild);
      while (els.eventLog.children.length > 60) {
        els.eventLog.removeChild(els.eventLog.lastChild);
      }
    });
    session.channel = channel;
    session.state = state;
    session.port = port;
    session.token = token;
    channel.connect().then((init) => {
      // 版本对齐口径(打包发布账 §四):壳不捆绑内核,连上后对协议版本
      // 对表,不合就把人话摆到状态条上——检测提示,不是拦截。
      session.compat = core.checkProtocolCompat(init && init.protocolVersion);
      note('握手', '协议 ' + (init && init.protocolVersion) +
        (session.compat.ok ? '' : ' [不合] ' + session.compat.hint +
          ' (内核 ' + ((init && init.lubancodeVersion) || '版本未报') + ')'));
      renderAll();
      // 现场对账:浏览器状态、页签账、journal 补账(重连同一条路)。
      channel.request('browser/status', {}).then((reply) => {
        if (reply.result && reply.result.launched) {
          state.browser.launched = true;
        }
        if (reply.result && reply.result.paused) {
          state.browser.paused = true;
        }
        return channel.request('browser/page/list', {});
      }).then((reply) => {
        // page/list 回的是页签行数组本身(不是 {pages:[...]})。
        for (const page of Array.isArray(reply.result) ? reply.result : []) {
          state.browser.pages[page.pageId] = page;
        }
        const pageId = activePage(state);
        if (pageId) {
          return state.backfillJournals(channel, pageId);
        }
      }).catch((e) => note('现场对账没成', e && e.message)).then(renderAll);
    }, (error) => {
      fail('连接', error);
      session.channel = null;
      session.state = null;
      session.compat = null;
      renderConnection();
    });
    renderConnection();
  }

  els.connectForm.onsubmit = (event) => {
    event.preventDefault();
    connect(parseInt(els.portInput.value, 10) || 8765, els.tokenInput.value.trim());
  };
  els.disconnectButton.onclick = () => {
    if (session.channel) {
      session.channel.close();
    }
    session.channel = null;
    session.state = null;
    session.compat = null;
    renderConnection();
  };

  els.newThreadButton.onclick = () => {
    session.channel.request('thread/start', {})
      .then((reply) => {
        session.state.currentThreadId = reply.result.threadId;
        renderAll();
      })
      .catch((e) => fail('开会话', e));
  };

  els.composer.onsubmit = (event) => {
    event.preventDefault();
    const text = els.sayInput.value.trim();
    if (!text || !session.state.currentThreadId) {
      return;
    }
    els.sayInput.value = '';
    session.state.upsertItem(session.state.currentThreadId, 'u' + (++session.userMessageSeq),
      { type: 'user', text: text, status: 'done' }, 'user');
    session.channel.request('turn/start', { threadId: session.state.currentThreadId, text: text })
      .catch((e) => fail('发话', e));
    renderAll();
  };

  els.interruptButton.onclick = () => {
    const threadId = session.state.currentThreadId;
    const turn = session.state.turnStatusByThread[threadId];
    session.channel.request('turn/interrupt', { threadId: threadId, turnId: turn && turn.turnId })
      .catch((e) => fail('打断', e));
  };

  els.browserStart.onclick = () => {
    session.channel.request('browser/start', { engine: 'chromium', headed: false })
      .catch((e) => fail('起场', e));
  };
  els.browserStop.onclick = () => {
    session.channel.request('browser/stop', {})
      .catch((e) => fail('收场', e));
  };
  els.browserPause.onclick = () => {
    session.channel.request('browser/pause', {}).catch((e) => fail('暂停', e));
  };
  els.browserResume.onclick = () => {
    session.channel.request('browser/resume', {}).catch((e) => fail('恢复', e));
  };

  els.navigateForm.onsubmit = (event) => {
    event.preventDefault();
    const url = els.urlInput.value.trim();
    if (!url) {
      return;
    }
    const pageId = activePage(session.state);
    const request = pageId
      ? session.channel.request('browser/page/navigate', { pageId: pageId, url: url })
      : session.channel.request('browser/page/open', { url: url });
    request.catch((e) => fail('导航', e));
  };
  els.openNewButton.onclick = () => {
    const url = els.urlInput.value.trim() || 'about:blank';
    session.channel.request('browser/page/open', { url: url, newPage: true }).catch((e) => fail('开页', e));
  };

  for (const tab of document.querySelectorAll('#panel-tabs button')) {
    tab.onclick = () => {
      for (const other of document.querySelectorAll('#panel-tabs button')) {
        other.className = '';
      }
      tab.className = 'active';
      els.consoleView.hidden = tab.dataset.tab !== 'console';
      els.networkView.hidden = tab.dataset.tab !== 'network';
      els.downloadsView.hidden = tab.dataset.tab !== 'downloads';
    };
  }

  els.screencastStart.onclick = () => {
    const params = { fps: 5 };
    const pageId = activePage(session.state);
    if (pageId) {
      params.pageId = pageId;
    }
    session.channel.request('browser/screencast/start', params).catch((e) => fail('开镜像流', e));
  };
  els.screencastStop.onclick = () => {
    const pageId = activePage(session.state);
    if (pageId) {
      session.channel.request('browser/screencast/stop', { pageId: pageId }).catch((e) => fail('停镜像流', e));
    }
  };

  els.snapshotButton.onclick = () => {
    const pageId = activePage(session.state);
    if (!pageId) {
      return;
    }
    session.channel.request('browser/snapshot', { pageId: pageId }).then((reply) => {
      session.lastSnapshotId = '';
      note('快照已受理', reply.result && reply.result.actionId);
    }).catch((e) => fail('快照', e));
    session.channel.waitForEvent('browser/action/completed', (params) => {
      return params.method === 'browser/snapshot' && params.ok && params.result && params.result.pageId === pageId;
    }, 45000).then((event) => {
      if (!event) {
        return;
      }
      session.lastSnapshotId = event.params.result.snapshotId;
      session.lastSnapshotRefs = core.parseSnapshotRefs(event.params.result.text);
      session.selectedRef = '';
      els.typeForm.hidden = true;
      renderAll();
    });
  };

  els.screenshotButton.onclick = () => {
    const pageId = activePage(session.state);
    if (pageId) {
      session.channel.request('browser/screenshot', { pageId: pageId }).catch((e) => fail('截图', e));
    }
  };

  els.typeForm.onsubmit = (event) => {
    event.preventDefault();
    const value = els.typeInput.value;
    if (!session.selectedRef || !value) {
      return;
    }
    els.typeInput.value = '';
    const params = { kind: 'type', ref: session.selectedRef, text: value, pageId: activePage(session.state) };
    if (session.lastSnapshotId) {
      params.snapshotId = session.lastSnapshotId;
    }
    session.channel.request('browser/action', params).catch((e) => fail('输入注入', e));
  };

  renderConnection();
})();
