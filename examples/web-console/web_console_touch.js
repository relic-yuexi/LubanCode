// LubanCode 参考前端——触屏外套(多前端外壳单阶段 E)。
//
// 一件薄外套,不是第四块拼图:协议路一行不碰(web_console_core/app 照旧),
// 只把触屏手势折成 DOM 事件与动作意图,Android WebView 壳(以及任何触屏
// 浏览器)靠它把"手指头"翻译成"点元素清单一行"。桌面浏览器没有它照活。
//
// 手势与协议的边界(照单子 §3.3,如实记账):
//   - tap        → 原生 click 照走(选中元素行);
//   - double-tap → 合成 dblclick(app.js 的 ondblclick → browser/action click);
//   - long-press → 合成 click 选中 + 聚焦 type 输入框(注入 type);
//   - swipe      → 不折协议:协议 1.1 的动作面只有 click|type|select|wait,
//                  没有 scroll——镜像/清单的滚动交给原生,壳不冒充。
// 纯逻辑(手势分类 + 意图折算)零 DOM,Node 冒烟
// (scripts/tests/app_shell_smoke.js)与页上同一份;DOM 接线只在浏览器。
'use strict';

(function (root) {
  // -------------------------------------------------------------------------
  // 阈值(像素按 CSS px 计;冒烟里造的事件用同一把尺)
  // -------------------------------------------------------------------------

  const THRESHOLDS = {
    tapSlopPx: 12,        // 位移小于它算"没挪窝"
    longPressMs: 500,     // 按住超过它算长按
    doubleTapMs: 320,     // 两次 tap 的间隔小于它算双击
    doubleTapSlopPx: 32,  // 两次 tap 的落点距离小于它算"同一处"
  };

  // -------------------------------------------------------------------------
  // 手势分类(纯函数)。events 是单指序列:
  //   [{t, phase:'down'|'move'|'up', x, y}, ...]  t 单位毫秒,坐标 CSS px。
  // state 是上一轮的 {lastTapAt, lastTapX, lastTapY}(双击判定要跨手势记),
  // 返回 {kind, ...细节, state}:kind 取
  //   'tap' | 'doubleTap' | 'longPress' | 'swipe' | 'pending' | null
  //   - pending:还在按着/不够判,接着喂;
  //   - null:喂的序列不成立(没从 down 起),调用方丢弃。
  // 接线层在 touchstart 起 longPressMs 的钟、move 超 slop 或 end 先到就撤,
  // 撤之前把当前序列喂进来——长按不等 up,到点即判。
  // -------------------------------------------------------------------------

  function classifyTouch(events, state, nowMs) {
    if (!events || !events.length || events[0].phase !== 'down') {
      return { kind: null, state: state || {} };
    }
    const first = events[0];
    const last = events[events.length - 1];
    const dx = last.x - first.x;
    const dy = last.y - first.y;
    const dist = Math.sqrt(dx * dx + dy * dy);
    const held = (nowMs !== undefined ? nowMs : last.t) - first.t;
    const s = state || {};
    let nextState = { lastTapAt: s.lastTapAt || 0, lastTapX: s.lastTapX || 0, lastTapY: s.lastTapY || 0 };

    if (last.phase === 'up') {
      if (dist > THRESHOLDS.tapSlopPx) {
        const horizontal = Math.abs(dx) >= Math.abs(dy);
        return {
          kind: 'swipe',
          dx: dx, dy: dy,
          horizontal: horizontal,
          state: nextState, // 滑动不算 tap,不搅双击账
        };
      }
      if (held >= THRESHOLDS.longPressMs) {
        return { kind: 'longPress', x: first.x, y: first.y, state: nextState };
      }
      const sinceLast = first.t - (s.lastTapAt || 0);
      const apart = Math.hypot(first.x - (s.lastTapX || 0), first.y - (s.lastTapY || 0));
      if (s.lastTapAt && sinceLast > 0 && sinceLast < THRESHOLDS.doubleTapMs &&
        apart <= THRESHOLDS.doubleTapSlopPx) {
        nextState = { lastTapAt: 0, lastTapX: 0, lastTapY: 0 }; // 双击已配对,清账
        return { kind: 'doubleTap', x: first.x, y: first.y, state: nextState };
      }
      nextState = { lastTapAt: last.t, lastTapX: first.x, lastTapY: first.y };
      return { kind: 'tap', x: first.x, y: first.y, state: nextState };
    }

    // 还按着:长按到点即判(不等 up);挪了窝就不是 tap/长按的路数了。
    if (dist <= THRESHOLDS.tapSlopPx && held >= THRESHOLDS.longPressMs) {
      return { kind: 'longPress', x: first.x, y: first.y, state: nextState };
    }
    return { kind: 'pending', state: nextState };
  }

  // -------------------------------------------------------------------------
  // 意图折算(纯函数)。gesture 是 classifyTouch 的结果,target 描述命中处:
  //   {kind:'elementRow', ref:'e12'}  — 元素清单一行(快照 ref);
  //   {kind:'other', label:'镜像图'}  — 别处(镜像图/空白/按钮)。
  // 返回意图,接线层照办:
  //   {type:'select'}          — 什么都不做,原生 click 会发(app.js 的 onclick);
  //   {type:'injectClick'}     — 合成 dblclick,走 app.js 的注入路;
  //   {type:'selectForTyping'} — 合成 click 选中,再聚焦 type 输入框;
  //   {type:'nativeOnly'}      — 交给原生,壳不冒充(协议没有对应动作)。
  // -------------------------------------------------------------------------

  function mapGesture(gesture, target) {
    if (!gesture || !gesture.kind) {
      return { type: 'nativeOnly' };
    }
    const onRow = target && target.kind === 'elementRow' && target.ref;
    if (gesture.kind === 'doubleTap') {
      return onRow ? { type: 'injectClick', ref: target.ref } : { type: 'nativeOnly' };
    }
    if (gesture.kind === 'longPress') {
      return onRow ? { type: 'selectForTyping', ref: target.ref } : { type: 'nativeOnly' };
    }
    if (gesture.kind === 'tap' || gesture.kind === 'swipe' || gesture.kind === 'pending') {
      return { type: 'select' }; // 原生 click/滚动照走,壳不加戏
    }
    return { type: 'nativeOnly' };
  }

  // -------------------------------------------------------------------------
  // DOM 接线(只在浏览器;冒烟不进这段)
  // -------------------------------------------------------------------------

  function rowAt(x, y) {
    const hit = document.elementFromPoint(x, y);
    for (let node = hit; node; node = node.parentElement) {
      if (node.tagName === 'LI' && node.dataset && node.dataset.ref) {
        return { kind: 'elementRow', ref: node.dataset.ref, node: node };
      }
    }
    return { kind: 'other', node: hit };
  }

  function fireDblClick(node) {
    node.dispatchEvent(new MouseEvent('dblclick', { bubbles: true, cancelable: true }));
  }

  function hint(message) {
    let box = document.getElementById('touch-hint');
    if (!box) {
      box = document.createElement('div');
      box.id = 'touch-hint';
      document.body.appendChild(box);
    }
    box.textContent = message;
    box.hidden = false;
    clearTimeout(box._timer);
    box._timer = setTimeout(() => { box.hidden = true; }, 2400);
  }

  function attachTouch(root) {
    const doc = root || document;
    const list = doc.getElementById('element-list');
    const mirror = doc.getElementById('mirror-image');
    if (!list || typeof doc.addEventListener !== 'function') {
      return false;
    }

    let events = [];
    let state = {};
    let pressTimer = 0;

    function classifyNow() {
      return classifyTouch(events, state, Date.now());
    }

    function act(gesture) {
      const target = gesture.x !== undefined ? rowAt(gesture.x, gesture.y) : { kind: 'other' };
      const intent = mapGesture(gesture, target);
      if (intent.type === 'injectClick') {
        fireDblClick(target.node);
        hint('双击 ' + intent.ref + ' → browser/action click(owner=user)');
      } else if (intent.type === 'selectForTyping') {
        target.node.dispatchEvent(new MouseEvent('click', { bubbles: true, cancelable: true }));
        const input = doc.getElementById('type-input');
        if (input) {
          input.focus();
        }
        hint('长按 ' + intent.ref + ' → 选中,输入文字注入 type');
      } else if (intent.type === 'nativeOnly' && gesture.kind === 'longPress') {
        hint('协议 1.1 快照没有坐标:点下方法元素清单的行,不是图');
      }
    }

    list.addEventListener('touchstart', (e) => {
      if (e.touches.length > 1) {
        events = []; // 多指不认,单指手势作废
        clearTimeout(pressTimer);
        return;
      }
      const t = e.touches[0];
      events = [{ t: Date.now(), phase: 'down', x: t.clientX, y: t.clientY }];
      clearTimeout(pressTimer);
      pressTimer = setTimeout(() => {
        const gesture = classifyNow();
        if (gesture.kind === 'longPress') {
          events.push({ t: Date.now(), phase: 'up', x: events[0].x, y: events[0].y });
          state = gesture.state;
          act(gesture);
          events = [];
        }
      }, THRESHOLDS.longPressMs);
    }, { passive: true });

    list.addEventListener('touchmove', (e) => {
      if (!events.length) {
        return;
      }
      const t = e.touches[0];
      events.push({ t: Date.now(), phase: 'move', x: t.clientX, y: t.clientY });
      const dx = t.clientX - events[0].x;
      const dy = t.clientY - events[0].y;
      if (Math.hypot(dx, dy) > THRESHOLDS.tapSlopPx) {
        clearTimeout(pressTimer); // 挪了窝,长按作废
      }
    }, { passive: true });

    const finish = (t) => {
      clearTimeout(pressTimer);
      if (!events.length) {
        return;
      }
      events.push({ t: Date.now(), phase: 'up', x: t.clientX, y: t.clientY });
      const gesture = classifyTouch(events, state);
      state = gesture.state || state;
      if (gesture.kind && gesture.kind !== 'pending') {
        act(gesture);
      }
      events = [];
    };

    list.addEventListener('touchend', (e) => {
      if (e.changedTouches && e.changedTouches.length) {
        finish(e.changedTouches[0]);
      }
    }, { passive: true });
    list.addEventListener('touchcancel', () => {
      clearTimeout(pressTimer);
      events = [];
    }, { passive: true });

    if (mirror) {
      // 镜像图上的手势如实说:协议没坐标,点了也不折协议动作。
      mirror.addEventListener('touchstart', () => hint('镜像图是只读的画面:动作点下方法元素清单'), { passive: true });
    }
    return true;
  }

  const api = {
    THRESHOLDS: THRESHOLDS,
    classifyTouch: classifyTouch,
    mapGesture: mapGesture,
    attachTouch: attachTouch,
  };

  if (typeof module !== 'undefined' && module.exports) {
    module.exports = api; // Node 冒烟走这条(与页上同一份)
  }
  root.LubanWebConsoleTouch = api;
  if (root.LubanWebConsole) {
    root.LubanWebConsole.touch = api;
  }

  if (typeof document !== 'undefined') {
    const start = () => attachTouch(document);
    if (document.readyState === 'loading') {
      document.addEventListener('DOMContentLoaded', start);
    } else {
      start();
    }
  }
})(typeof window !== 'undefined' ? window : globalThis);
