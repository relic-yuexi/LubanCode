// 统一回放接口(骨架拆解批五乙,病十六后半):五套台账的回放规矩归一。
//
// 收的是"规矩",不是载荷:loop 的 loop_task_v1/loop_tick_v1、goal 的
// goal_v1 族、workflow 的 RunJournal+checkpoint,事件载荷与折叠语义各域
// 保留(域字段、状态机推进、恢复后的默认动作各家照旧)。这份合同只钉
// 三件公共事:
//   - 信封(Envelope):一行台账解析出的公共骨架——族名/事件名/次序号/
//     时间戳;域字段全在 payload 里原样过境,信封不偷看、不裁剪。
//   - 次序:append-only 台账,文件序即回放序;行内带 seq 的域(workflow
//     journal)喂折叠口前按 seq 稳定排序——半截尾行跳过后仍单调。不带
//     seq 的行(loop/goal 的 session 行)文件序原样(stable_sort 对全 0
//     等价不动)。
//   - 恢复入口:坏行跳过不废整场(五套台账从各写各的 try/catch 收成
//     这里一份——编解码给 nullopt、折叠口返 false 或抛异常,都算这一条
//     不认,账面交 Stats,整场照跑)。
//
// 落 runtime/ 与批五甲的横切件(BudgetGate/RetryBackoff/IdAuthority)同屋:
// 三家消费方(loop 装配层、goal coordinator、workflow journal)依赖方向
// 全部合法。sessions/ 不反向依赖 runtime(老规矩)——goal 行的中立解析
// (ParseGoalEvent)留在 sessions 层,信封折账在 coordinator 这头做。
//
// IdAuthority(批五甲)管"发号同源",本件管"回放同规":两件合起来是
// 病十六"台账方言五种"的公共半边;各域的事件模型(信封里的 payload)
// 有意不收——那是域语义,收了反而没处安放折叠差异。

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::runtime::replay {

// 一行台账的公共骨架。域字段(task_id/goal_id/node_id/attempt/…)全在
// payload 里原样过境——信封只钉五家都有的那几件。
struct Envelope {
    std::string family;    // 台账族(行顶层 type):loop_task_v1/goal_iteration_v1/
                           // journal 的 node_completed 一类
    std::string event;     // 族内事件名:loop/goal 行的 event 字段;journal 行
                           // 与 family 同值(它的 type 就是事件名)
    std::uint64_t seq = 0; // 行内次序号;journal 带,session 行不带(0=按文件序)
    std::int64_t timestamp_ms = 0;
    nlohmann::json payload = nlohmann::json::object();  // 域载荷原样
};

// 域编解码:一行台账 -> 信封。坏行/异族行给 nullopt(调用方不必再写
// try/catch,本件恢复入口兜着)。
using LineCodec = std::function<std::optional<Envelope>(const std::string& line)>;

// 域折叠口:一枚信封 -> 重建一笔账。返回 false = 这条不认(跳过);
// 抛异常同 false(域折叠里 json 取错类型一类,不废整场)。
using Fold = std::function<bool(const Envelope& envelope)>;

// 一场恢复的账面:过眼/收进/跳过(跳过 = 解不动 + 折叠口拒收)。
struct Stats {
    int lines = 0;
    int replayed = 0;
    int skipped = 0;
};

// 恢复入口(一份):行进、信封出、按序喂折叠口、坏行跳过不废整场。
Stats ReplayLedgerLines(const std::vector<std::string>& lines, const LineCodec& codec, const Fold& fold);

// 已解析信封的整批回放(goal 的 RestoreFromArchive 形状:事件先由存档层
// 解成域事件,这里只走同一条次序/跳过/账面规矩)。
Stats ReplayEnvelopes(std::vector<Envelope> envelopes, const Fold& fold);

}  // namespace lubancode::runtime::replay
