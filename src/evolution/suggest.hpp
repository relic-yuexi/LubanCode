// 自进化闭环阶段 7:有限自动建议——只提示候选机会,不自动起草、测试或
// 安装(§七"什么时候才值得提炼"的自动提示五条门槛,逐条落)。
//
// 触发只是"提示":亮一行建议,用户点头才走既有 /evolve propose 路。建议
// 引擎不碰候选仓的写口、不碰起草器、不碰评测——它只读观察账与候选仓,
// 判门槛、记两笔账(开关账 suggest.json 与命中账 suggest.jsonl)。
//
// 缺省关闭是铁律(§十五"缺省不开自动起草,也不开自动安装"):开关文件
// 缺失、读不动、写坏了,一律当关——不猜开。关着的时候,/evolve status
// 不评估建议、不写命中账,连新材料都不为建议收集(观察账本身照旧,那是
// 阶段 1 的显式采集,不是建议引擎的后台)。
//
// 五条门槛(todo §七"自动提示至少要满足"):
//   门一 两次以上独立任务证据——同指纹簇内不同 source_id 的观察 >= 2 条
//        (同一场里的重试折在同一条观察里,凑不成"独立任务");
//   门二 输入、产物、验收大体同形——指纹本身就是"目标口述 + 验收口述 +
//        折叠工具序列"的归一哈希(日期/网址/绝对路径已抽象),同指纹即
//        同形;簇里还得真有一条形状(工具/节点序列非空),空形状无从同形;
//   门三 非偶然——>=2 条不同 source_id 的成功观察(成功须带验证证据,
//        阶段 1 采集器把过关);单场一次成功可能是撞上的,两场各自走通
//        才算路子;
//   门四 无同 fingerprint 的 pending/rejected 候选——观察账的拒绝指纹账
//        与候选仓的既有候选(在途或被拒)一并挡门,用户拒绝后不死缠;
//   门五 能说明比现有 Memory、Skill 或 Package 多解决什么——簇的形状
//        里有做法(工具/节点序列 >=1 步)才值得起包;没步骤的簇是一句
//        事实或偏好,Memory 就装得下,不劝人封包。
//
// 命中账(suggest.jsonl,只追加):每行一笔事件——"shown" 是过五门亮出来
// 的提示,"accepted" 是用户真去 propose 落了候选。命中率与接受率从这本
// 账现算:命中率 = 出过提示的指纹数 / 达到门一的簇数;接受率 = 出过
// 提示又真起草的指纹数 / 出过提示的指纹数。
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "evolution/candidate.hpp"
#include "evolution/observation.hpp"
#include "evolution/observation_store.hpp"

namespace lubancode::evolution {

// 门槛的数值面(可 inspect:/evolve suggest 裸跑时亮出来)。
struct SuggestThresholds {
    int min_independent_tasks = 2;      // 门一:独立任务(不同 source_id)下限
    int min_independent_successes = 2;  // 门三:独立成功场数下限
    int min_shape_steps = 1;            // 门五:形状里的最少步数(空形状不劝)
};

// 一簇(同指纹)的五门判定账。why_not 逐条人话,给 inspect 与测试看。
struct SuggestionVerdict {
    std::string fingerprint;
    int cluster_size = 0;      // 簇内观察条数
    int independent_tasks = 0;  // 不同 source_id 的观察数(门一)
    int independent_successes = 0;  // 不同 source_id 且 outcome=success 的数(门三)
    int shape_steps = 0;       // 首条观察的形状步数(工具/节点序列长度)
    bool gate_tasks = false;           // 门一
    bool gate_shape = false;           // 门二
    bool gate_not_accidental = false;  // 门三
    bool gate_no_pending_or_rejected = true;  // 门四(blocked 命中即 false)
    bool gate_benefit = false;         // 门五
    bool eligible = false;             // 五门全过
    std::string benefit_line;          // 门五的话:比现有件多解决什么
    std::vector<std::string> why_not;  // 没过的门逐条人话(过则空)
    std::string representative_obs_id;  // 提示行指给 /evolve propose 的观察 id
    std::string summary;                // 簇首摘要(提示行用)
};

// 判一簇的五门(纯函数)。cluster 须同指纹(调用方聚好);blocked 是门四
// 的挡门指纹集(观察账拒绝指纹 + 候选仓既有候选的来源指纹,见
// CollectBlockedFingerprints)。
SuggestionVerdict AssessSuggestion(const std::vector<EvolutionObservation>& cluster,
                                   const std::vector<std::string>& blocked,
                                   const SuggestThresholds& thresholds = SuggestThresholds{});

// 门四的挡门指纹集(纯函数只读两本账):
//   - 观察账的 rejected 账(被拒指纹,内容未变不再劝);
//   - 候选仓里每只候选:来源观察(recording_ids -> 观察 id)还查得到的,
//     按其指纹挡门——在途(pending)挡,active/staged 挡(已提炼过),
//     rejected 也挡(与拒绝账同源,双保险)。查不到来源指纹的候选不硬猜。
std::vector<std::string> CollectBlockedFingerprints(const ObservationStore& observations,
                                                    const CandidateStore& candidates);

// 观察里"形状步数":details.tools(录制)或 details.nodes(run)的序列
// 长度;没有形状账给 0(纯函数,门二/门五共用)。
int ShapeStepsOf(const EvolutionObservation& observation);

// ---------------------------------------------------------------------------
// 开关账:<evolution 根>/suggest.json —— {"schema":1,"enabled":bool}。
// 缺文件/坏 JSON/schema 不对/enabled 不是布尔,一律 false(缺省关闭)。
// ---------------------------------------------------------------------------

bool LoadSuggestEnabled(const std::filesystem::path& evolution_root);
// 写开关(建不出目录/写不动给错误人话)。写 false 永远写得成。
std::optional<std::string> SaveSuggestEnabled(const std::filesystem::path& evolution_root,
                                              bool enabled);

// ---------------------------------------------------------------------------
// 命中账:<evolution 根>/suggest.jsonl —— 只追加,一行一事件(schema 1)。
//   {"schema":1,"type":"shown","fingerprint":…,"at":…,"cluster_size":N,
//    "benefit":…,"obs_id":…}
//   {"schema":1,"type":"accepted","fingerprint":…,"at":…,"candidate_id":…}
// 坏行/半截行读取时跳过,不废整账(与观察账、评测账同约定)。
// ---------------------------------------------------------------------------

struct SuggestEvent {
    int schema = 1;
    std::string type;        // "shown" / "accepted"
    std::string fingerprint;
    std::string at;          // ISO 8601 UTC
    int cluster_size = 0;    // shown:判门时的簇大小
    std::string benefit;     // shown:门五的话
    std::string obs_id;      // shown:提示行指的代表观察
    std::string candidate_id;  // accepted:落下的候选 id
};

class SuggestLedger {
public:
    explicit SuggestLedger(std::filesystem::path file);

    // 追加一笔(只追加;建目录/开文件失败返回错误人话)。
    std::optional<std::string> Append(const SuggestEvent& event);
    // 读全账(坏行跳过;文件不存在给空表)。
    std::vector<SuggestEvent> Load() const;

    // 命中率与接受率的账面(从事件账现算):
    //   shown_events / accepted_events  事件笔数(一笔提示一笔接受)
    //   shown_fingerprints              出过提示的指纹数(去重)
    //   accepted_fingerprints           出过提示又真起草的指纹数(去重)
    //   acceptance_rate                 accepted_fingerprints / shown_fingerprints
    //                                   (没提示过给 -1,不冒充 0)
    struct Stats {
        int shown_events = 0;
        int accepted_events = 0;
        int shown_fingerprints = 0;
        int accepted_fingerprints = 0;
        double acceptance_rate = -1.0;
    };
    Stats ComputeStats() const;

    // 这枚指纹出过提示、还没记过接受?(propose 回执用它补 accepted 事件,
    // 同一指纹多次 propose 只记头一笔——接受率按指纹算,不按次数灌水。)
    bool HasOpenSuggestion(const std::string& fingerprint) const;

private:
    std::filesystem::path file_;
};

}  // namespace lubancode::evolution
