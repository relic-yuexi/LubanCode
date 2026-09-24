// 一场 session 的 Journal -> UsageSample 装配(Token 账本单 A2)。
//
// /usage 的读侧:认领 session 目录,把两代账读回汇成一包 samples——
// v3 场走 ReadV3Ledger/WalkSessionTree(owner 驱动 + 树内递归),v2 场把
// main/subagents/workflows 各条 stream 逐条 ProjectUsage。纯读——writer
// 持句柄照读(journal 以共享读开),不 flush、不回写、不补造事实。
//
// active session 直接读已提交高水位,成色由调用方标 provisional;这里只
// 如实汇报 session.json 的 status 与读到的东西。
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "accounting/usage_sample.hpp"

namespace lubancode::accounting {

struct SessionUsageRead {
    bool ok = false;
    std::string error_code;   // usage.* 稳定码
    std::string message;      // 人话(报告原样打)
    std::string session_id;   // 目录名(session.json 读不到时兜底)
    std::string workspace_key;
    std::string status;       // session.json 的 status;读不到写 unknown
    std::string format;       // "v2" | "v3":本场账的格式(旧历史另标,不混
                               // v3 完整率分母);读不出格式留空(错误路径)
    std::vector<UsageSample> samples;   // 各 stream 按 run 字典序、stream 内出现序
    std::vector<std::string> warnings;  // 投影 warnings 透传(usage.purpose_missing…)
    // session.json 存在且 status=closed:封口账;其余(active/读不到)调
    // 用方一律按 provisional 口径标,不在这里猜"应该封了"。
    bool sealed() const { return status == "closed"; }
};

// 列一场 session 的全部 stream 文件(字典序)。两代布局并出(V3-GAP-01):
// v2 布局件(main.jsonl、平铺 subagents、workflows)+ v3 布局件(<id>.jsonl
// 主账与递归子 session 目录;主账按首行 schemaVersion 验明才算)。清单只
// 认盘上文件;怎么读、怎么去重归 ReadSessionUsage。目录不存在给
// nullopt(调用方按"没有这场 session"报,不算账错)。
std::optional<std::vector<std::filesystem::path>> ListSessionStreams(
    const std::filesystem::path& session_dir);

// 读一场 session 并投影。session 目录不存在 → ok=false、
// error_code=usage.session_not_found,不产残账。某条 stream 坏/版本混写
// → 该条 stream 的 samples 不算数,warning 点名,其余照读——一场 session
// 一条坏 stream 不至于整场没账,但坏处必须看得见。
//
// 格式分派(T06/V3-GAP-01):先探 v3(<id>.jsonl)再走 v2(main.jsonl)。
// v3 主账验卷不过 → ok=false(不拿空样本伪装零消耗);子 session 坏/缺
// → warning 点名(partial),其余照读。v3 只计本树(本账 + 递归子 session),
// resume 源链不在树内——physical spend 按实际发生去重,祖先不重复计费;
// 需要 lineage 汇总由调用方显式选范围。两种主账并存 → <id>.jsonl 验明
// v3 即以 v3 为准并账:旧 main.jsonl/平铺子账只补 v3 开账之前的段,之后
// 一笔不计(防双计),并账与弃账各自点名;workflows 编排账不并(V3-GAP-05
// 另管)。验不明(异版本/坏首行)仍按格式冲突拒读,不猜格式。
SessionUsageRead ReadSessionUsage(const std::filesystem::path& session_dir);

}  // namespace lubancode::accounting
