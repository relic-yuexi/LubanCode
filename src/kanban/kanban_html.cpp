// Kanban 看板单:自包含 HTML 模板与拼装。合同见 kanban_html.hpp。
#include "kanban/kanban_html.hpp"

namespace lubancode::kanban {

namespace {

// 页面模板。%%KANBAN_DATA%% 是唯一占位符,替换为 JSON 字面量。
const char kTemplate[] = R"KANBAN(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>LubanCode 看板</title>
<style>
:root {
  --bg: #0d1117;
  --panel: #161b26;
  --panel-2: #1c2333;
  --border: #2a3348;
  --text: #e6edf3;
  --muted: #8b98ac;
  --accent: #d2a8ff;
  --green: #3fb950;
  --blue: #58a6ff;
  --gray: #8b98ac;
  --red: #f85149;
  --amber: #d29922;
}
* { box-sizing: border-box; }
body {
  margin: 0;
  background: radial-gradient(1200px 500px at 20% -10%, #1b2340 0%, var(--bg) 55%) fixed;
  color: var(--text);
  font-family: "Segoe UI", "Microsoft YaHei", system-ui, -apple-system, sans-serif;
  font-size: 14px;
}
header {
  padding: 18px 24px 12px;
  border-bottom: 1px solid var(--border);
  background: rgba(13, 17, 23, .75);
  backdrop-filter: blur(8px);
  position: sticky;
  top: 0;
  z-index: 10;
}
.brand { display: flex; align-items: baseline; gap: 12px; flex-wrap: wrap; }
.brand h1 {
  margin: 0;
  font-size: 20px;
  letter-spacing: .5px;
  background: linear-gradient(90deg, #79c0ff, #d2a8ff);
  -webkit-background-clip: text;
  background-clip: text;
  color: transparent;
}
.stats { color: var(--muted); font-size: 13px; }
.stats b { color: var(--text); }
.toolbar { display: flex; gap: 10px; margin-top: 12px; flex-wrap: wrap; align-items: center; }
.toolbar input[type="search"], .toolbar select {
  background: var(--panel);
  border: 1px solid var(--border);
  color: var(--text);
  border-radius: 8px;
  padding: 7px 12px;
  font-size: 13px;
  outline: none;
}
.toolbar input[type="search"] { width: 280px; }
.toolbar input[type="search"]:focus, .toolbar select:focus { border-color: var(--accent); }
.view-toggle { display: flex; border: 1px solid var(--border); border-radius: 8px; overflow: hidden; }
.view-toggle button {
  background: transparent; color: var(--muted); border: 0; padding: 7px 16px;
  cursor: pointer; font-size: 13px;
}
.view-toggle button.active { background: var(--panel-2); color: var(--text); }
.diag {
  margin: 12px 24px 0; padding: 10px 14px; border: 1px solid var(--amber);
  border-radius: 8px; color: var(--amber); background: rgba(210,153,34,.08); font-size: 13px;
}
.board { display: flex; gap: 16px; padding: 16px 24px 32px; align-items: flex-start; }
.column {
  flex: 1 1 0; min-width: 260px;
  background: rgba(22, 27, 38, .6);
  border: 1px solid var(--border);
  border-radius: 12px;
  display: flex; flex-direction: column;
  max-height: calc(100vh - 170px);
}
.column-head {
  display: flex; align-items: center; gap: 8px;
  padding: 12px 14px; border-bottom: 1px solid var(--border);
  font-weight: 600;
}
.dot { width: 9px; height: 9px; border-radius: 50%; }
.count {
  margin-left: auto; color: var(--muted); font-weight: 400; font-size: 12px;
  background: var(--panel-2); border-radius: 10px; padding: 1px 8px;
}
.cards { padding: 10px; overflow-y: auto; display: flex; flex-direction: column; gap: 10px; }
.card {
  background: var(--panel);
  border: 1px solid var(--border);
  border-left: 3px solid var(--gray);
  border-radius: 10px;
  padding: 10px 12px;
  cursor: pointer;
  transition: transform .12s ease, border-color .12s ease, box-shadow .12s ease;
}
.card:hover { transform: translateY(-2px); border-color: var(--accent); box-shadow: 0 6px 18px rgba(0,0,0,.35); }
.card.running { border-left-color: var(--green); }
.card.closed { border-left-color: var(--blue); }
.card.archived { border-left-color: var(--gray); opacity: .82; }
.card-title {
  font-weight: 600; line-height: 1.4; margin-bottom: 6px;
  display: -webkit-box; -webkit-line-clamp: 2; -webkit-box-orient: vertical; overflow: hidden;
  word-break: break-all;
}
.badges { display: flex; gap: 6px; flex-wrap: wrap; margin-bottom: 8px; }
.badge {
  font-size: 11px; color: var(--muted);
  background: var(--panel-2); border: 1px solid var(--border);
  border-radius: 6px; padding: 1px 7px;
  max-width: 100%; overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
}
.badge.project { color: var(--accent); }
.badge.oneshot { color: var(--amber); }
.badge.damaged { color: var(--red); }
.card-foot { display: flex; justify-content: space-between; color: var(--muted); font-size: 12px; }
.empty { color: var(--muted); text-align: center; padding: 28px 0 20px; font-size: 13px; }

.projects { display: grid; grid-template-columns: repeat(auto-fill, minmax(320px, 1fr)); gap: 16px; padding: 16px 24px 32px; }
.project-card {
  background: var(--panel); border: 1px solid var(--border); border-radius: 12px;
  padding: 16px; cursor: pointer;
  transition: transform .12s ease, border-color .12s ease, box-shadow .12s ease;
}
.project-card:hover { transform: translateY(-2px); border-color: var(--accent); box-shadow: 0 6px 18px rgba(0,0,0,.35); }
.project-name { font-size: 16px; font-weight: 700; margin-bottom: 4px; word-break: break-all; }
.project-root { color: var(--muted); font-size: 12px; word-break: break-all; margin-bottom: 12px; }
.project-stats { display: flex; gap: 14px; color: var(--muted); font-size: 12px; flex-wrap: wrap; }
.project-stats b { color: var(--text); }

.overlay {
  position: fixed; inset: 0; background: rgba(4, 6, 10, .6);
  backdrop-filter: blur(3px); z-index: 100;
  display: flex; align-items: center; justify-content: center; padding: 24px;
}
.modal {
  background: var(--panel); border: 1px solid var(--border); border-radius: 14px;
  width: min(720px, 100%); max-height: 85vh; overflow-y: auto;
  padding: 22px 24px; box-shadow: 0 18px 60px rgba(0,0,0,.5);
}
.modal h2 { margin: 0 0 4px; font-size: 17px; word-break: break-all; padding-right: 32px; }
.modal .sub { color: var(--muted); font-size: 12px; margin-bottom: 16px; word-break: break-all; }
.kv { display: grid; grid-template-columns: 110px 1fr; gap: 8px 14px; font-size: 13px; }
.kv dt { color: var(--muted); }
.kv dd { margin: 0; word-break: break-all; }
.quote {
  margin: 16px 0 0; padding: 12px 14px; background: var(--panel-2);
  border-left: 3px solid var(--accent); border-radius: 8px;
  white-space: pre-wrap; word-break: break-word; font-size: 13px; line-height: 1.6;
  max-height: 240px; overflow-y: auto;
}
.modal-close {
  float: right; background: transparent; border: 0; color: var(--muted);
  font-size: 20px; cursor: pointer; line-height: 1;
}
.modal-close:hover { color: var(--text); }
.session-row {
  display: flex; justify-content: space-between; gap: 10px; align-items: baseline;
  padding: 8px 10px; border-radius: 8px; cursor: pointer; border: 1px solid transparent;
}
.session-row:hover { background: var(--panel-2); border-color: var(--border); }
.session-row .t { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.session-row .d { color: var(--muted); font-size: 12px; white-space: nowrap; }
.section-label { color: var(--muted); font-size: 12px; margin: 18px 0 6px; text-transform: uppercase; letter-spacing: 1px; }
</style>
</head>
<body>
<header>
  <div class="brand">
    <h1>LubanCode 看板</h1>
    <div class="stats" id="stats"></div>
  </div>
  <div class="toolbar">
    <input type="search" id="search" placeholder="搜索标题 / 首句 / 会话 ID…">
    <select id="projectFilter"></select>
    <div class="view-toggle">
      <button id="viewBoard" class="active">看板</button>
      <button id="viewProjects">项目</button>
    </div>
  </div>
</header>
<div class="diag" id="diag" hidden></div>
<main class="board" id="board"></main>
<main class="projects" id="projects" hidden></main>
<div class="overlay" id="overlay" hidden><div class="modal" id="modal"></div></div>
<script>
"use strict";
const DATA = %%KANBAN_DATA%%;
const projectByKey = new Map((DATA.projects || []).map(p => [p.key, p]));
const allSessions = DATA.sessions || [];

const COLS = [
  { id: "running",  label: "进行中", color: "#3fb950" },
  { id: "closed",   label: "已结束", color: "#58a6ff" },
  { id: "archived", label: "已归档", color: "#8b98ac" },
];

function el(tag, cls, text) {
  const node = document.createElement(tag);
  if (cls) node.className = cls;
  if (text !== undefined && text !== null) node.textContent = text;
  return node;
}

function columnOf(s) {
  if (s.status === "running" || s.status === "preparing") return "running";
  if (s.archived || s.status === "archived") return "archived";
  return "closed";
}

function projectName(key) {
  const p = projectByKey.get(key);
  if (p && p.name) return p.name;
  if (p && p.identity_root) {
    const parts = p.identity_root.split(/[\\/]/).filter(Boolean);
    return parts[parts.length - 1] || p.identity_root;
  }
  return key || "(未知项目)";
}

function fmtTime(ms) {
  if (!ms) return "—";
  return new Date(ms).toLocaleString("zh-CN", { hour12: false });
}

function fmtRel(ms) {
  if (!ms) return "—";
  const diff = Date.now() - ms;
  if (diff < 0) return fmtTime(ms);
  const min = Math.floor(diff / 60000);
  if (min < 1) return "刚刚";
  if (min < 60) return min + " 分钟前";
  const hr = Math.floor(min / 60);
  if (hr < 24) return hr + " 小时前";
  const day = Math.floor(hr / 24);
  if (day < 30) return day + " 天前";
  return fmtTime(ms);
}

function sessionTitle(s) {
  const t = (s.title || s.first_user_text || "").trim();
  return t || "(无标题)";
}

let query = "";
let projectFilter = "";

function passesFilter(s) {
  if (projectFilter && s.workspace_key !== projectFilter) return false;
  if (!query) return true;
  const hay = ((s.title || "") + " " + (s.first_user_text || "") + " " + s.id + " " +
               projectName(s.workspace_key)).toLowerCase();
  return hay.includes(query);
}

function filteredSessions() {
  return allSessions.filter(passesFilter);
}

function renderStats() {
  const running = allSessions.filter(s => columnOf(s) === "running").length;
  const stats = document.getElementById("stats");
  stats.textContent = "";
  const b1 = el("b", null, String(allSessions.length));
  const b2 = el("b", null, String(running));
  stats.append(document.createTextNode(DATA.projects.length + " 个项目 · "),
               b1, document.createTextNode(" 场会话 · "), b2, document.createTextNode(" 进行中"));
  if (DATA.version) {
    stats.append(document.createTextNode(" · v" + DATA.version));
  }
}

function makeCard(s) {
  const col = columnOf(s);
  const card = el("div", "card " + col);
  card.append(el("div", "card-title", sessionTitle(s)));
  const badges = el("div", "badges");
  badges.append(el("span", "badge project", projectName(s.workspace_key)));
  if (s.model) badges.append(el("span", "badge", s.model));
  if (s.run_kind === "one_shot") badges.append(el("span", "badge oneshot", "单发"));
  if (s.run_kind_unknown) badges.append(el("span", "badge", "种类未知"));
  if (s.damaged) badges.append(el("span", "badge damaged", "受损"));
  card.append(badges);
  const foot = el("div", "card-foot");
  foot.append(el("span", null, fmtRel(s.updated_at_ms)),
              el("span", null, "💬 " + (s.message_count || 0)));
  card.append(foot);
  card.addEventListener("click", () => openSessionModal(s));
  return card;
}

function renderBoard() {
  const board = document.getElementById("board");
  board.textContent = "";
  const sessions = filteredSessions();
  for (const col of COLS) {
    const colSessions = sessions.filter(s => columnOf(s) === col.id);
    const column = el("section", "column");
    const head = el("div", "column-head");
    const dot = el("span", "dot");
    dot.style.background = col.color;
    head.append(dot, el("span", null, col.label),
                el("span", "count", String(colSessions.length)));
    column.append(head);
    const cards = el("div", "cards");
    if (colSessions.length === 0) {
      cards.append(el("div", "empty", "空"));
    } else {
      for (const s of colSessions) cards.append(makeCard(s));
    }
    column.append(cards);
    board.append(column);
  }
}

function projectStats(key) {
  const list = allSessions.filter(s => s.workspace_key === key);
  const running = list.filter(s => columnOf(s) === "running").length;
  const messages = list.reduce((acc, s) => acc + (s.message_count || 0), 0);
  const lastActive = list.reduce((acc, s) => Math.max(acc, s.updated_at_ms || 0), 0);
  return { total: list.length, running, messages, lastActive };
}

function renderProjects() {
  const grid = document.getElementById("projects");
  grid.textContent = "";
  const projects = (DATA.projects || []).filter(p => {
    if (projectFilter && p.key !== projectFilter) return false;
    if (!query) return true;
    const hay = ((p.name || "") + " " + (p.identity_root || "")).toLowerCase();
    return hay.includes(query);
  });
  if (projects.length === 0) {
    grid.append(el("div", "empty", "没有匹配的项目"));
    return;
  }
  for (const p of projects) {
    const st = projectStats(p.key);
    const card = el("div", "project-card");
    card.append(el("div", "project-name", p.name || projectName(p.key)));
    card.append(el("div", "project-root", p.identity_root || p.key));
    const stats = el("div", "project-stats");
    const add = (label, value) => {
      const span = el("span");
      span.append(el("b", null, String(value)), document.createTextNode(" " + label));
      stats.append(span);
    };
    add("场会话", st.total);
    add("进行中", st.running);
    add("条消息", st.messages);
    stats.append(el("span", null, "最近活跃:" + fmtRel(st.lastActive || p.last_opened_at_ms)));
    card.append(stats);
    card.addEventListener("click", () => openProjectModal(p));
    grid.append(card);
  }
}

function kvRow(dt, dd) {
  return [el("dt", null, dt), el("dd", null, dd)];
}

function openSessionModal(s) {
  const modal = document.getElementById("modal");
  modal.textContent = "";
  const close = el("button", "modal-close", "✕");
  close.addEventListener("click", closeModal);
  modal.append(close);
  modal.append(el("h2", null, sessionTitle(s)));
  modal.append(el("div", "sub", s.id + " · " + projectName(s.workspace_key)));
  const kv = el("dl", "kv");
  const statusText = { running: "进行中", preparing: "准备中", closed: "已结束", archived: "已归档" }[s.status] || s.status || "未知";
  const runKind = s.run_kind_unknown ? "(种类未知)"
    : (s.run_kind === "one_shot" ? "单发(one_shot)" : "主会话");
  for (const [k, v] of [
    ["状态", statusText + (s.damaged ? "(账尾受损)" : "")],
    ["项目", projectName(s.workspace_key)],
    ["类型", runKind],
    ["模型", s.model || "—"],
    ["工作目录", s.cwd || "—"],
    ["创建时间", fmtTime(s.created_at_ms)],
    ["最后活动", fmtTime(s.updated_at_ms) + "(" + fmtRel(s.updated_at_ms) + ")"],
    ["消息数", String(s.message_count || 0)],
    ["事件数", String(s.event_count || 0)],
    ["存储目录", s.session_dir || "—"],
  ]) kv.append(...kvRow(k, v));
  modal.append(kv);
  if (s.first_user_text) {
    modal.append(el("div", "section-label", "首条提问"));
    modal.append(el("div", "quote", s.first_user_text));
  }
  showOverlay();
}

function openProjectModal(p) {
  const st = projectStats(p.key);
  const modal = document.getElementById("modal");
  modal.textContent = "";
  const close = el("button", "modal-close", "✕");
  close.addEventListener("click", closeModal);
  modal.append(close);
  modal.append(el("h2", null, p.name || projectName(p.key)));
  modal.append(el("div", "sub", p.key));
  const kv = el("dl", "kv");
  for (const [k, v] of [
    ["身份类型", p.identity_kind || "—"],
    ["身份根", p.identity_root || "—"],
    ["创建时间", fmtTime(p.created_at_ms)],
    ["最近打开", fmtTime(p.last_opened_at_ms) + "(" + fmtRel(p.last_opened_at_ms) + ")"],
    ["会话总数", String(st.total)],
    ["进行中", String(st.running)],
    ["消息总数", String(st.messages)],
  ]) kv.append(...kvRow(k, v));
  modal.append(kv);
  if (p.checkouts && p.checkouts.length) {
    modal.append(el("div", "section-label", "登记的 checkout"));
    for (const c of p.checkouts) {
      const row = el("div", "session-row");
      row.append(el("span", "t", c.root), el("span", "d", fmtRel(c.last_seen_at_ms)));
      modal.append(row);
    }
  }
  const list = allSessions.filter(s => s.workspace_key === p.key);
  if (list.length) {
    modal.append(el("div", "section-label", "会话(" + list.length + ")"));
    for (const s of list) {
      const row = el("div", "session-row");
      row.append(el("span", "t", sessionTitle(s)),
                 el("span", "d", fmtRel(s.updated_at_ms) + " · 💬 " + (s.message_count || 0)));
      row.addEventListener("click", () => openSessionModal(s));
      modal.append(row);
    }
  }
  showOverlay();
}

function showOverlay() { document.getElementById("overlay").hidden = false; }
function closeModal() { document.getElementById("overlay").hidden = true; }

function renderProjectFilter() {
  const sel = document.getElementById("projectFilter");
  sel.textContent = "";
  const all = el("option", null, "全部项目");
  all.value = "";
  sel.append(all);
  for (const p of DATA.projects || []) {
    const opt = el("option", null, p.name || projectName(p.key));
    opt.value = p.key;
    sel.append(opt);
  }
}

function renderAll() {
  renderStats();
  renderBoard();
  renderProjects();
}

document.getElementById("search").addEventListener("input", e => {
  query = e.target.value.trim().toLowerCase();
  renderAll();
});
document.getElementById("projectFilter").addEventListener("change", e => {
  projectFilter = e.target.value;
  renderAll();
});
document.getElementById("viewBoard").addEventListener("click", () => switchView("board"));
document.getElementById("viewProjects").addEventListener("click", () => switchView("projects"));
function switchView(view) {
  document.getElementById("board").hidden = view !== "board";
  document.getElementById("projects").hidden = view !== "projects";
  document.getElementById("viewBoard").classList.toggle("active", view === "board");
  document.getElementById("viewProjects").classList.toggle("active", view === "projects");
}
document.getElementById("overlay").addEventListener("click", e => {
  if (e.target.id === "overlay") closeModal();
});
document.addEventListener("keydown", e => {
  if (e.key === "Escape") closeModal();
});

if (DATA.diagnostic) {
  const diag = document.getElementById("diag");
  diag.hidden = false;
  diag.textContent = "索引读取提示:" + DATA.diagnostic;
}
renderProjectFilter();
renderAll();
</script>
</body>
</html>
)KANBAN";

// JSON 嵌进 <script>:把 "</" 折成 "<\/"(JSON 字符串里合法的转义),
// 防数据里的 "</script>" 提前闭合脚本块。
std::string EscapeJsonForInlineScript(std::string json) {
    std::string out;
    out.reserve(json.size() + 16);
    for (std::size_t i = 0; i < json.size(); ++i) {
        if (json[i] == '<' && i + 1 < json.size() && json[i + 1] == '/') {
            out += "<\\/";
            ++i;
            continue;
        }
        out += json[i];
    }
    return out;
}

}  // namespace

std::string RenderKanbanHtml(const nlohmann::json& data) {
    const std::string payload = EscapeJsonForInlineScript(data.dump());
    std::string html(kTemplate);
    const std::string marker = "%%KANBAN_DATA%%";
    const std::size_t pos = html.find(marker);
    if (pos != std::string::npos) {
        html.replace(pos, marker.size(), payload);
    }
    return html;
}

}  // namespace lubancode::kanban
