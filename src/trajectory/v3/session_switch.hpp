// 新会话写 v3 的最小开关(P2 接线评估落地;单子 §1.5 大重构口径 +
// 分期 P2"读取侧按 v3 解析",P3 显示侧未齐,默认不切)。
//
// 开关语义:只管"新会话选哪种轨迹格式";老会话(已存在的 v2 目录)
// 永远照 v2 读写,与本开关无关。默认关 = 新会话仍写 v2。
//
// 接线点清单(实施留待 P3/后续棒,读取侧已就绪,P2 不强行接):
//
//   1. 开新会话(写侧入口)
//      src/runtime/trajectory_session.cpp 的延迟开卷装配(P0-C:正式
//      .jsonl 由首枚 run.started 提交事务独占建卷):开 = 在该处以
//      V3Writer::Start 建 sessions/<id>/<id>.jsonl,关 = 现行 v2
//      main.jsonl。这是唯一需要二选一的点。
//   2. 会话清单与目录发现(读回路)
//      src/trajectory/session_manager.cpp:SessionManager::CreateSession/
//      ResumeSession/ListSessions 目前认 v2 目录布局(main.jsonl +
//      manifest)。v3 会话须并列识别 sessions/<id>/<id>.jsonl + 首行
//      schemaVersion==3。牵动产品读回路 → 记给 P3,不在 P2 动。
//   3. resume 读回路
//      session_manager.cpp ResumeSession 的 FoldStreamReplay/effective
//      conversation 投影是 v2 专用;v3 resume 走本仓
//      trajectory::v3::ProjectResume(历史索引/模型输入/执行状态三恢复)。
//      两回路并存,按源目录格式分派,不迁移旧档(§1.5)。
//   4. subagent/工作流子账
//      src/runtime/trajectory_session.cpp 子账(subagents/<run>.jsonl)
//      同样受开关管辖:开 = 子账走 v3::SubagentSpawn 五步;工作流
//      workflow.jsonl 编排账不迁移(v3 只管 session 主账/子账)。
//   5. 显示层(纯 P3)
//      终端/app-server/browser 的旧史滚动、压缩标记、分页详情消费
//      HistoryTimeline/CompactMarkerView 投影;隐藏消息(display.hidden)
//      的展开规则也在此层,不进读写合同。
//
// 判据(翻默认前须全绿):v3 Continue 全家福 + P2 读取侧矩阵全过;
// 显示侧(P3)吃上 HistoryTimeline;api/wire 四角色(P4)合同测试绿。
#pragma once

namespace lubancode::trajectory::v3 {

// 新会话是否写 v3。环境变量 LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS=1 开;
// 其余任何值(含未设)关。每次调用现读,进程内不缓存——开关翻动只影响
// 之后开的新会话。
bool NewSessionV3WriteEnabled();

}  // namespace lubancode::trajectory::v3
