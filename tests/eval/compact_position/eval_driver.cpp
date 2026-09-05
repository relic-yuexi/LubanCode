// 实验 B1(compact 位置探针,Q2 量化评测单 §二 B1)的三处理驱动 + 问答判卷。
//
// 三铁律(单子 §二 B1 记账段,2026-09-05 工头令):
//   1) 问答驱动判卷:同一份底稿出三版"请求视图"(FULL/microcompact/compact),
//      对每枚 needle 提一问,判卷只看**模型答没答对**(归一化后比对回答与
//      期望值/冲突旧值)——不再扫视图找原词充当 compact 臂成绩(摘要本分
//      就是换说法,子串判卷把好摘要冤枉死)。装置阶段问答方是脚本化假后端
//      fake-grounded:忠实于视图作答(看见新值答新值、只见旧值答旧值、
//      都不见答"未提及"),验管道与判卷逻辑;真跑路 --real 见 README
//      (问答模型 = 实验模型 ccmoon/gpt-5.6-luna,装置阶段零真请求)。
//   2) 触发条件贴生产:三处理阈值全用产品默认——microcompact 走
//      StructuralCompressionOptions{} 默认(long_result_bytes=8192 /
//      preview_bytes=256 / min_compressible_bytes=512);compact 走
//      CompactOptions{} 默认 + kDefaultHotZoneTokens=12000,预算窗口
//      nullopt 与生产 BuildCompactOptions 在"路由与模型目录都查不到窗口"
//      时的形态一致(账里 budget_window_tokens=null 如实记)。驱动不自设
//      任何新数。
//   3) needle 分层:金账 layer=contract(用户约定,落 user 消息,按产品
//      设计该进 manifest 逐字收编)与 evidence(工具输出里的事实,本分是
//      被摘要)分开记账,"设计如此"与"意外丢失"不混锅。
//
// 三处理:
//   FULL         原文直发(零处理,对照基线)。grounded 满分问答在 FULL 上
//                应全 hit——装置完整性锚(任一 miss 即装置坏,exit 1)。
//   microcompact 产品无损结构压缩折叠路 agent::CompressWorkingView:
//                超长 read 结果被换成头尾 256B 预览,长段中段 needle 真丢,
//                grounded 模型答不出(lost);旧值活在短段而新值被折掉时,
//                模型照旧值作答(stale 陷阱)。视图级子串在场在此臂(与
//                FULL 臂)只作辅助诊断列——折叠是机械截断,原词在场确实是
//                好信号;但成绩只认问答。
//   compact      六栏摘要路 agent::Compact + BuildCompactedHistory。装置
//                阶段摘要采样用 FakeStreamingBackend 替身(固定形态六栏 +
//                manifest,过四道验收),summary_fake=true 如实标注;问答
//                判卷照常跑(grounded 模型在假摘要视图上只见热区),该臂
//                装置数字只证管道,语义命中率待真模型。
//
// 判卷(确定性,独立自检):
//   归一化(ASCII 小写、剥空白/全角空格/零宽)后子串比对**模型回答**:
//   recall:   回答含期望值 → hit;不含 → lost。
//   conflict: 回答含新值 → hit(回答顺带提及旧值不算错,与"新值声明里
//             提及旧值"同骨);不含新值而含旧值 → stale(superseded 旧值
//             不得作答);两值都不含 → lost。
//
// 用法:
//   eval_compact_position [--drafts <dir>] [--results <dir>] [--self-check]
//   (--real 为真跑路预留,装置阶段未接线:零真请求、零真钥匙。)
// 缺省取编译期 LUBANCODE_EVAL_COMPACT_POSITION_ROOT(源码树
//   tests/eval/compact_position),底稿在其 results/drafts/,原始账落其
//   results/raw_position_probe.jsonl,三处理视图落 results/views/(审计用)。
// 退出码:装置自身坏(自检/断言不过)非 0;正常跑完 0——判卷 miss 是账,
// 不是错。

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/compact.hpp"
#include "agent/context.hpp"
#include "agent/context_events.hpp"
#include "api/types.hpp"
#include "fake_backend.hpp"

namespace {

namespace fs = std::filesystem;
using nlohmann::json;
using lubancode::api::Message;
using lubancode_eval::FakeStreamingBackend;

constexpr const char* kExperiment = "compact_position";
constexpr const char* kFakeModel = "fake-eval-model";
constexpr const char* kFakeProvider = "fake";
// 装置问答方:脚本化 grounded 假后端(读视图作答,零真请求)。
constexpr const char* kAnsweringModel = "fake-grounded-v1";

std::string PathToUtf8(const fs::path& path) {
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

std::string NowIso8601Utc() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &now);
#else
    gmtime_r(&tm, &now);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

// ---- 底稿装载 ---------------------------------------------------------------

struct SegmentDef {
    std::string tool_use_id;
    std::string path;
    std::string text;
    std::string length_class;
};

struct UserTurnDef {
    int slot = 0;
    std::string text;
};

struct NeedleDef {
    std::string fact_id;
    std::string layer;       // contract | evidence(分层铁律 3)
    std::string probe_kind;  // recall | conflict
    std::string carrier;     // tool_result | user_turn
    std::string lang;
    std::string key;         // 配置项键(问题面)
    std::string question;    // 问题(金账同款,判卷题面)
    int position_pct = 0;
    double actual_position_pct = 0.0;
    int seg_index = -1;      // evidence:落段
    int old_seg_index = -1;
    int slot = -1;           // contract:落 user 消息序位
    int old_slot = -1;
    double offset_pct_in_seg = 0.0;
    std::string seg_length_class;
    std::string expected_value;
    std::string old_value;  // conflict 专属;recall 为空
};

struct Draft {
    std::string draft_id;
    std::string lang;
    int repeat = 0;
    long long seed = 0;
    std::vector<SegmentDef> segments;
    std::vector<UserTurnDef> user_turns;
    std::vector<NeedleDef> needles;
};

std::string StringOr(const json& node, const char* key) {
    if (node.contains(key) && node[key].is_string()) {
        return node[key].get<std::string>();
    }
    return std::string();
}

// null 安全取值:recall 行的 old_value/old_seg_index 在金账里是 null,
// json::value() 撞 null 会抛 type_error,这里缺键与 null 都走默认。
int IntOr(const json& node, const char* key, int fallback) {
    if (node.contains(key) && node[key].is_number_integer()) {
        return node[key].get<int>();
    }
    return fallback;
}

double DoubleOr(const json& node, const char* key, double fallback) {
    if (node.contains(key) && node[key].is_number()) {
        return node[key].get<double>();
    }
    return fallback;
}

Draft LoadDraft(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        throw std::runtime_error("draft 打不开: " + PathToUtf8(path));
    }
    json root;
    try {
        in >> root;
    } catch (const std::exception& exc) {
        throw std::runtime_error("draft 不是合法 JSON: " + PathToUtf8(path) + ": " + exc.what());
    }
    Draft draft;
    draft.draft_id = StringOr(root, "draft_id");
    draft.lang = StringOr(root, "lang");
    draft.repeat = root.value("repeat", 0);
    draft.seed = root.value("seed", 0LL);
    for (const auto& seg : root.at("segments")) {
        SegmentDef def;
        def.tool_use_id = StringOr(seg, "tool_use_id");
        def.path = StringOr(seg, "path");
        def.text = StringOr(seg, "text");
        def.length_class = StringOr(seg, "length_class");
        draft.segments.push_back(std::move(def));
    }
    if (root.contains("user_turns")) {
        for (const auto& turn : root.at("user_turns")) {
            UserTurnDef def;
            def.slot = IntOr(turn, "slot", 0);
            def.text = StringOr(turn, "text");
            draft.user_turns.push_back(std::move(def));
        }
    }
    for (const auto& needle : root.at("needles")) {
        NeedleDef def;
        def.fact_id = StringOr(needle, "fact_id");
        def.layer = StringOr(needle, "layer");
        def.probe_kind = StringOr(needle, "probe_kind");
        def.carrier = StringOr(needle, "carrier");
        def.lang = StringOr(needle, "lang");
        def.key = StringOr(needle, "key");
        def.question = StringOr(needle, "question");
        def.position_pct = IntOr(needle, "position_pct", 0);
        def.actual_position_pct = DoubleOr(needle, "actual_position_pct", 0.0);
        def.seg_index = IntOr(needle, "seg_index", -1);
        def.old_seg_index = IntOr(needle, "old_seg_index", -1);
        def.slot = IntOr(needle, "slot", -1);
        def.old_slot = IntOr(needle, "old_slot", -1);
        def.offset_pct_in_seg = DoubleOr(needle, "offset_pct_in_seg", 0.0);
        def.seg_length_class = StringOr(needle, "seg_length_class");
        def.expected_value = StringOr(needle, "expected_value");
        def.old_value = StringOr(needle, "old_value");
        draft.needles.push_back(std::move(def));
    }
    if (draft.segments.empty() || draft.needles.empty()) {
        throw std::runtime_error("draft 缺段或缺 needle: " + PathToUtf8(path));
    }
    return draft;
}

// ---- 底稿 → api::Message 历史 ----------------------------------------------
//
// 形状仿真实长会话:user 开工 → [assistant tool_use(read_file) → user
// tool_result] × N(每 4 段一条 assistant 收口 + user 续读,开新 turn——
// turn 粒度取小,compact 的 12k-token 热区才装得下尾部两三 turn,位置轴
// 才有区分度)→ assistant 收口 → user 终问(热区锚点)。tool_use/
// tool_result 紧邻配对,与 BuildEventLedger 的原子配对形状一致。user 开工
// 与续读的正文来自底稿 user_turns(contract needle 嵌在这里)。

struct HistoryLayout {
    // 每段 read 结果所在的 user 消息下标(evidence needle 的 hot_kept 判定)。
    std::vector<std::size_t> result_message_of_segment;
    // 每 slot 的 user 文本消息下标(contract needle 的 hot_kept 判定)。
    std::vector<std::size_t> user_message_of_slot;
    std::size_t message_count = 0;
};

std::vector<Message> BuildHistory(const Draft& draft, HistoryLayout* layout) {
    std::vector<Message> history;
    history.reserve(draft.segments.size() * 2 + 8);
    const bool zh = draft.lang == "zh";

    auto user_text_of_slot = [&](std::size_t slot) -> std::string {
        for (const UserTurnDef& turn : draft.user_turns) {
            if (static_cast<std::size_t>(turn.slot) == slot) {
                return turn.text;
            }
        }
        // 兜底(手造底稿没给全):沿用造稿器默认文案。
        return slot == 0 ? (zh ? "任务:通读 docs/draft/ 下的工程文档,整理其中的关键配置项与变更记录。"
                               : "Task: read through the engineering docs and collect key settings.")
                         : (zh ? "继续,留意配置项的变更记录。" : "Continue; watch for setting changes.");
    };

    Message open;
    open.role = lubancode::api::Role::User;
    open.content.push_back(lubancode::api::TextBlock{user_text_of_slot(0)});
    history.push_back(std::move(open));
    if (layout != nullptr) {
        layout->user_message_of_slot.push_back(0);
    }

    constexpr std::size_t kBatch = 4;
    std::size_t turn_slot = 1;
    for (std::size_t i = 0; i < draft.segments.size(); ++i) {
        const SegmentDef& seg = draft.segments[i];
        if (i > 0 && i % kBatch == 0) {
            Message close;
            close.role = lubancode::api::Role::Assistant;
            close.content.push_back(lubancode::api::TextBlock{
                zh ? "这一批文档读完了,要点已记,继续读下一批。" : "Batch done; notes taken, reading on."});
            history.push_back(std::move(close));
            Message next;
            next.role = lubancode::api::Role::User;
            next.content.push_back(lubancode::api::TextBlock{user_text_of_slot(turn_slot)});
            history.push_back(std::move(next));
            if (layout != nullptr) {
                layout->user_message_of_slot.push_back(history.size() - 1);
            }
            ++turn_slot;
        }
        Message call;
        call.role = lubancode::api::Role::Assistant;
        lubancode::api::ToolUseBlock use;
        use.id = seg.tool_use_id;
        use.name = "read_file";
        use.input = json{{"path", seg.path}, {"offset", 0}, {"limit", 4096}};
        call.content.push_back(std::move(use));
        history.push_back(std::move(call));

        Message result;
        result.role = lubancode::api::Role::User;
        lubancode::api::ToolResultBlock block;
        block.tool_use_id = seg.tool_use_id;
        block.content = seg.text;
        result.content.push_back(std::move(block));
        history.push_back(std::move(result));
        if (layout != nullptr) {
            layout->result_message_of_segment.push_back(history.size() - 1);
        }
    }

    Message done;
    done.role = lubancode::api::Role::Assistant;
    done.content.push_back(lubancode::api::TextBlock{
        zh ? "全部文档读完了,可以汇总。" : "All docs read; ready to summarize."});
    history.push_back(std::move(done));
    Message question;
    question.role = lubancode::api::Role::User;
    question.content.push_back(lubancode::api::TextBlock{
        zh ? "文档读完了。请汇总关键配置项与它们的最新的取值。"
           : "Docs are read. Summarize the key settings and their latest values."});
    history.push_back(std::move(question));

    if (layout != nullptr) {
        layout->message_count = history.size();
    }
    return history;
}

// 视图 → 判卷文本投影(TextBlock 与 ToolResultBlock 正文;工具入参 JSON
// 不进——事实值只出现在结果正文与 user 消息里)。
std::string ViewText(const std::vector<Message>& view) {
    std::string out;
    for (const auto& message : view) {
        for (const auto& block : message.content) {
            if (const auto* text = std::get_if<lubancode::api::TextBlock>(&block)) {
                out += text->text;
                out += '\n';
            } else if (const auto* result = std::get_if<lubancode::api::ToolResultBlock>(&block)) {
                out += result->content;
                out += '\n';
            }
        }
    }
    return out;
}

std::size_t HistoryBytes(const std::vector<Message>& history) {
    return ViewText(history).size();
}

// ---- 归一化与匹配(问答判卷的底座) -------------------------------------------

// 归一化:ASCII 小写;剥 ASCII 空白、全角空格 U+3000、不换行空格 U+00A0、
// 零宽空格 U+200B。两侧同规则,匹配不受措辞排版影响。
std::string NormalizeForMatch(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size();) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            ++i;
            continue;
        }
        if (c >= 'A' && c <= 'Z') {
            out.push_back(static_cast<char>(c - 'A' + 'a'));
            ++i;
            continue;
        }
        if (c == 0xC2 && i + 1 < in.size() && static_cast<unsigned char>(in[i + 1]) == 0xA0) {
            i += 2;  // U+00A0
            continue;
        }
        if (c == 0xE3 && i + 2 < in.size() && static_cast<unsigned char>(in[i + 1]) == 0x80 &&
            static_cast<unsigned char>(in[i + 2]) == 0x80) {
            i += 3;  // U+3000
            continue;
        }
        if (c == 0xE2 && i + 2 < in.size() && static_cast<unsigned char>(in[i + 1]) == 0x80 &&
            static_cast<unsigned char>(in[i + 2]) == 0x8B) {
            i += 3;  // U+200B
            continue;
        }
        out.push_back(in[i]);
        ++i;
    }
    return out;
}

bool ContainsValue(const std::string& normalized_haystack, const std::string& value) {
    return normalized_haystack.find(NormalizeForMatch(value)) != std::string::npos;
}

// ---- 问答判卷(铁律 1:只看模型回答) ------------------------------------------

// 判卷结果:verdict ∈ hit | stale | lost。比对对象是**模型回答**,不是视图。
//   recall:   回答含期望值 → hit;不含 → lost。
//   conflict: 回答含新值 → hit(回答顺带提及旧值不算错——新值声明本就
//             "已由旧变更为新");不含新值而含旧值 → stale(superseded 旧值
//             不得作答,拿旧值答与答不出同记 miss,stale 单列);
//             两值都不含 → lost。
struct AnswerVerdict {
    bool hit = false;
    const char* verdict = "lost";
    bool answer_has_new = false;
    bool answer_has_old = false;
};

AnswerVerdict GradeAnswer(const std::string& normalized_answer, const NeedleDef& needle) {
    AnswerVerdict out;
    out.answer_has_new = ContainsValue(normalized_answer, needle.expected_value);
    out.answer_has_old =
        !needle.old_value.empty() && ContainsValue(normalized_answer, needle.old_value);
    if (needle.probe_kind == "conflict") {
        if (out.answer_has_new) {
            out.hit = true;
            out.verdict = "hit";
        } else if (out.answer_has_old) {
            out.verdict = "stale";  // 照旧值作答:错,单列
        }
        return out;
    }
    out.hit = out.answer_has_new;
    out.verdict = out.answer_has_new ? "hit" : "lost";
    return out;
}

// ---- 装置问答方:脚本化 grounded 假后端 ---------------------------------------
//
// 形态:忠实于所见视图的"满分模型"——看见新值答新值(整句,带措辞噪声,
// 判卷归一化后抽值)、只见旧值答旧值、都不见答"未提及"(句中绝不出现
// 任何值)。不发请求、不读钥匙;真跑时换实验模型(ccmoon/gpt-5.6-luna)
// 读视图答题,判卷器不动。
struct ViewPresence {
    bool new_in_view = false;
    bool old_in_view = false;
};

ViewPresence CheckViewPresence(const std::string& normalized_view, const NeedleDef& needle) {
    ViewPresence out;
    out.new_in_view = ContainsValue(normalized_view, needle.expected_value);
    out.old_in_view =
        !needle.old_value.empty() && ContainsValue(normalized_view, needle.old_value);
    return out;
}

std::string AskGroundedFake(const NeedleDef& needle, const ViewPresence& presence) {
    const bool zh = needle.lang == "zh";
    const std::string& value = presence.new_in_view   ? needle.expected_value
                               : presence.old_in_view ? needle.old_value
                                                      : std::string();
    if (!value.empty()) {
        return zh ? "看记录,配置项 " + needle.key + " 的取值为 " + value + "。"
                  : "Per the records, the value of '" + needle.key + "' is " + value + ".";
    }
    return zh ? "记录里没有提到这个取值,我答不上来。"
              : "I cannot find that value anywhere in the records.";
}

// ---- 三处理(铁律 2:全用产品默认,驱动不自设新数) ---------------------------

struct TreatmentOutput {
    std::string name;  // FULL | microcompact | compact
    std::vector<Message> view;
    std::size_t original_bytes = 0;
    std::size_t view_bytes = 0;
    // microcompact 折叠账
    std::size_t offloaded_results = 0;
    std::size_t duplicate_groups = 0;
    std::size_t superseded_observations = 0;
    std::size_t offloaded_saved_bytes = 0;
    // compact 管道账(预算参数如实记:生产默认形态)
    bool summary_fake = false;
    std::size_t compact_requests = 0;
    std::size_t kept_messages = 0;
    std::vector<std::size_t> kept_indices;
    // compact 预算窗口:nullopt = 窗口未知(生产 BuildCompactOptions 在路由
    // 与模型目录都查不到窗口时同样留空;fake 模型无目录条目,即此形态)。
    bool budget_window_known = false;
    std::size_t budget_window_tokens = 0;
    bool ok = false;
    std::string error;
};

// compact 假后端替身:固定形态的六栏摘要 + 末尾 ```json manifest。内容是假
// 的(不取自底稿),只保证过 Compact 的四道验收(≥40 码点、manifest 可解析、
// goal 非空、历史比摘要大)。语义待真模型跑。
std::string FakeSixColumnSummary() {
    return "## 任务目标\n"
           "(装置假摘要)通读示例工程文档,整理关键配置项与变更记录。\n"
           "\n"
           "## 已证实的事实\n"
           "(装置假摘要——内容不取自底稿,只验六栏+manifest 管道;语义待真模型跑。)\n"
           "\n"
           "## 关键决策\n"
           "(无)\n"
           "\n"
           "## 涉及文件与符号\n"
           "(无)\n"
           "\n"
           "## 关键命令与结果\n"
           "(无)\n"
           "\n"
           "## 未完成事项\n"
           "(无)\n"
           "\n"
           "```json\n"
           "{\"goal\": \"通读示例工程文档并整理关键配置项\", \"constraints\": [], "
           "\"open_items\": [], \"next_action\": \"继续整理剩余文档\"}\n"
           "```\n";
}

TreatmentOutput ApplyTreatment(const std::string& name, const std::vector<Message>& history) {
    TreatmentOutput out;
    out.name = name;
    out.original_bytes = HistoryBytes(history);
    if (name == "FULL") {
        out.view = history;
        out.view_bytes = out.original_bytes;
        out.ok = true;
        return out;
    }
    if (name == "microcompact") {
        // 产品默认参数(agent/context_events.hpp 的 StructuralCompressionOptions
        // 默认值):long_result_bytes=8192、preview_bytes=256、
        // min_compressible_bytes=512。一次性"从头定形"视图(fresh memo),
        // 不带 artifact 仓(store=nullptr,渲染无 artifact_id 变体)。
        lubancode::agent::StructuralCompressionOptions options{};
        lubancode::agent::StructuralCompressionStats stats;
        out.view = lubancode::agent::CompressWorkingView(history, options, stats);
        out.offloaded_results = stats.offloaded_results;
        out.duplicate_groups = stats.duplicate_groups;
        out.superseded_observations = stats.superseded_observations;
        out.offloaded_saved_bytes = stats.offloaded_saved_bytes;
        out.view_bytes = HistoryBytes(out.view);
        out.ok = true;
        return out;
    }
    if (name == "compact") {
        FakeStreamingBackend backend;
        backend.scripts = {lubancode_eval::TextScript(
            FakeSixColumnSummary(), lubancode::api::Usage{5120, 384, 0, 0, 0})};
        // 生产默认形态:CompactOptions{} 的预算窗口是 nullopt(窗口未知,
        // 不做拦截、如实记"未按窗口校验");output_reserve_tokens=4096 与
        // protocol_headroom_tokens=2048 用 CompactBudget 默认——生产路
        // BuildCompactOptions 仅在窗口已知时才按 BuildContextBudgetPlan 重算
        // headroom,窗口未知即默认值,与本处同形。真跑路(--real)接真后端
        // 工厂时应照生产路由查模型目录窗口(README 写明)。
        lubancode::agent::CompactOptions options;
        out.budget_window_known = options.budget.window_tokens.has_value();
        out.budget_window_tokens = options.budget.window_tokens.value_or(0);
        const auto summary = lubancode::agent::Compact(backend, kFakeModel, history, options);
        if (!summary.has_value()) {
            out.error = "compact 假后端路失败(装置坏): " + summary.error().message;
            return out;
        }
        out.compact_requests = backend.captured_requests.size();
        if (out.compact_requests != 1) {
            out.error = "compact 假后端路应恰发一次请求,实发 " + std::to_string(out.compact_requests);
            return out;
        }
        out.view = lubancode::agent::BuildCompactedHistory(history, summary->archive,
                                                           lubancode::agent::kDefaultHotZoneTokens,
                                                           &out.kept_indices);
        out.kept_messages = out.kept_indices.size();
        out.view_bytes = HistoryBytes(out.view);
        out.summary_fake = true;
        out.ok = true;
        return out;
    }
    out.error = "未知处理: " + name;
    return out;
}

// ---- 自检(问答判卷单测 + 失败注入负路径 + 管道;eval 冒烟章法) ---------------

bool RunSelfChecks(std::string* error) {
    const auto fail = [error](const std::string& what) {
        if (error != nullptr) {
            *error = what;
        }
        return false;
    };

    // 1) 归一化:小写 + 剥空白/全角空格。
    if (NormalizeForMatch("  The SETTING is Harbor-Blue. ") != "thesettingisharbor-blue.") {
        return fail("NormalizeForMatch 大小写/空白归一不对");
    }
    if (NormalizeForMatch("取值为\xe3\x80\x80青梧-1009\n") != "取值为青梧-1009") {
        return fail("NormalizeForMatch 全角空格没剥掉");
    }

    // 2) 问答判卷:比对对象是模型回答。
    {
        NeedleDef needle;
        needle.layer = "evidence";
        needle.probe_kind = "recall";
        needle.expected_value = "青梧-1009";
        // 整句作答(带措辞噪声)→ 归一化抽值 → hit。
        const AnswerVerdict hit = GradeAnswer(
            NormalizeForMatch("看记录,配置项 runtime.knob_00 的取值为 青梧-1009,以此为准。"), needle);
        if (!hit.hit || std::string(hit.verdict) != "hit" || !hit.answer_has_new) {
            return fail("recall 整句作答应判 hit");
        }
        // 大小写与空格变形回答 → hit。
        NeedleDef en_needle;
        en_needle.layer = "evidence";
        en_needle.probe_kind = "recall";
        en_needle.expected_value = "Qingwu-1009";
        const AnswerVerdict spaced = GradeAnswer(
            NormalizeForMatch("The value is  QINGWU-1009  per the records."), en_needle);
        if (!spaced.hit) {
            return fail("recall 回答大小写/空格变形应仍判 hit");
        }
        // 失败注入:答"未提及" → lost;答别的值 → lost。
        const AnswerVerdict absent = GradeAnswer(
            NormalizeForMatch("记录里没有提到这个取值,我答不上来。"), needle);
        if (absent.hit || std::string(absent.verdict) != "lost" || absent.answer_has_new) {
            return fail("recall 注入'答不出'应判 lost(负路径)");
        }
        const AnswerVerdict wrong = GradeAnswer(NormalizeForMatch("取值为 青梧-4242。"), needle);
        if (wrong.hit || std::string(wrong.verdict) != "lost") {
            return fail("recall 注入'答错值'应判 lost(负路径)");
        }
    }
    {
        NeedleDef needle;
        needle.layer = "evidence";
        needle.probe_kind = "conflict";
        needle.expected_value = "玄序-6100";
        needle.old_value = "玄序-3100";
        // 答新值 → hit;回答顺带提及旧值("已由旧变更为新")→ 仍 hit。
        const AnswerVerdict hit = GradeAnswer(
            NormalizeForMatch("配置项取值为 玄序-6100。"), needle);
        if (!hit.hit || !hit.answer_has_new) {
            return fail("conflict 答新值应判 hit");
        }
        const AnswerVerdict mention_old = GradeAnswer(
            NormalizeForMatch("该配置项已由 玄序-3100 变更为 玄序-6100,现为 玄序-6100。"), needle);
        if (!mention_old.hit || !mention_old.answer_has_old) {
            return fail("conflict 回答含新值(顺带提旧值)应判 hit");
        }
        // 失败注入:答旧值 → stale(hit 必须为 false);答未提及 → lost;
        // 答无关值 → lost。
        const AnswerVerdict stale = GradeAnswer(
            NormalizeForMatch("看记录,配置项的取值为 玄序-3100。"), needle);
        if (stale.hit || std::string(stale.verdict) != "stale" || !stale.answer_has_old) {
            return fail("conflict 注入'答旧值'应判 stale 且不算 hit(负路径)");
        }
        const AnswerVerdict absent = GradeAnswer(
            NormalizeForMatch("记录里没有提到这个取值,我答不上来。"), needle);
        if (absent.hit || std::string(absent.verdict) != "lost") {
            return fail("conflict 注入'答不出'应判 lost(负路径)");
        }
        const AnswerVerdict wrong = GradeAnswer(NormalizeForMatch("取值为 玄序-9999。"), needle);
        if (wrong.hit || std::string(wrong.verdict) != "lost") {
            return fail("conflict 注入'答无关值'应判 lost(负路径)");
        }
    }

    // 3) grounded 假问答后端:忠实于视图作答,三类回答与判卷闭环。
    {
        NeedleDef needle;
        needle.layer = "evidence";
        needle.probe_kind = "conflict";
        needle.lang = "zh";
        needle.key = "deploy.baseline_00";
        needle.expected_value = "玄序-6100";
        needle.old_value = "玄序-3100";
        // 视图含新值 → 回答含新值 → hit。
        ViewPresence both;
        both.new_in_view = true;
        both.old_in_view = true;
        const std::string answer_new = AskGroundedFake(needle, both);
        const AnswerVerdict v_new = GradeAnswer(NormalizeForMatch(answer_new), needle);
        if (!v_new.hit) {
            return fail("grounded:视图含新值时回答应判 hit");
        }
        // 视图仅含旧值 → 回答含旧值不含新 → stale(主链 stale 陷阱的机理)。
        ViewPresence only_old;
        only_old.old_in_view = true;
        const std::string answer_old = AskGroundedFake(needle, only_old);
        if (ContainsValue(NormalizeForMatch(answer_old), needle.expected_value)) {
            return fail("grounded:只见旧值时回答不得含新值");
        }
        const AnswerVerdict v_old = GradeAnswer(NormalizeForMatch(answer_old), needle);
        if (v_old.hit || std::string(v_old.verdict) != "stale") {
            return fail("grounded:只见旧值时应答旧值、判卷 stale");
        }
        // 视图两值都无 → 回答不含任何值 → lost。
        ViewPresence none;
        const std::string answer_none = AskGroundedFake(needle, none);
        if (ContainsValue(NormalizeForMatch(answer_none), needle.expected_value) ||
            ContainsValue(NormalizeForMatch(answer_none), needle.old_value)) {
            return fail("grounded:'未提及'回答里不得出现任何值");
        }
        const AnswerVerdict v_none = GradeAnswer(NormalizeForMatch(answer_none), needle);
        if (v_none.hit || std::string(v_none.verdict) != "lost") {
            return fail("grounded:两值都不见时应判 lost");
        }
    }

    // 4) 微型管道:一段超长(needle 居中)+ 一段短(needle 在场)+ 同
    //    path 新旧读取两对(medium 对验 NewVersion;短旧长新对验 stale
    //    陷阱)+ user 消息里的 contract needle 一对。全程走"grounded 作答
    //    → 问答判卷",与主链同一条路。
    {
        Draft mini;
        mini.draft_id = "mini";
        mini.lang = "zh";
        const std::string filler_line = "构建日志:模块 042 编译完成,警告 3 条,耗时 217 毫秒。\n";
        const std::string pad_line = "巡检记录:节点 041 的磁盘水位 38%,仍在安全带内。\n";
        SegmentDef long_seg;
        long_seg.tool_use_id = "u_long";
        long_seg.path = "docs/mini/long.md";
        long_seg.length_class = "long";
        {
            while (long_seg.text.size() < 10 * 1024) {
                long_seg.text += filler_line;
            }
            const std::size_t mid = long_seg.text.size() / 2;
            long_seg.text.insert(mid, "【关键事实】配置项 mini.long 的取值核定为 青梧-9001,以此为准。\n");
        }
        SegmentDef short_seg;
        short_seg.tool_use_id = "u_short";
        short_seg.path = "docs/mini/short.md";
        short_seg.length_class = "short";
        short_seg.text = "巡检记录:节点 007 的磁盘水位 41%,仍在安全带内。\n"
                         "【关键事实】配置项 mini.short 的取值核定为 青梧-9002,以此为准。\n";
        // conflict 对 A:同 path 两版 medium(都 ≥512B),新版全文在场。
        SegmentDef old_seg;
        old_seg.tool_use_id = "u_old";
        old_seg.path = "config/mini.toml";
        old_seg.length_class = "medium";
        old_seg.text = "【配置基线】配置项 mini.port 初始设定为 玄序-3100。\n";
        SegmentDef new_seg;
        new_seg.tool_use_id = "u_new";
        new_seg.path = "config/mini.toml";
        new_seg.length_class = "medium";
        new_seg.text = "【配置变更】配置项 mini.port 已由 玄序-3100 变更为 玄序-6100,以本条为准。\n";
        // NewVersion 判据要求正文 ≥ min_compressible_bytes(512B):给同 path
        // 的新旧两版各垫足上下文,让"文件改版"分支真触发。
        for (std::size_t pad = 0; pad < 24; ++pad) {
            old_seg.text += pad_line;
            new_seg.text += pad_line;
        }
        // conflict 对 B:旧版短段(≥512B 不折,旧值活)、新版长段(needle
        // 居中,折叠丢新值)——grounded 模型只见旧值,照旧值作答 → stale。
        SegmentDef old_short_seg;
        old_short_seg.tool_use_id = "u_old_b";
        old_short_seg.path = "config/mini_b.toml";
        old_short_seg.length_class = "short";
        old_short_seg.text = "【配置基线】配置项 mini.gate 初始设定为 白鹭-7600。\n";
        while (old_short_seg.text.size() < 600) {
            old_short_seg.text += pad_line;
        }
        SegmentDef new_long_seg;
        new_long_seg.tool_use_id = "u_new_b";
        new_long_seg.path = "config/mini_b.toml";
        new_long_seg.length_class = "long";
        {
            std::string body;
            while (body.size() < 10 * 1024) {
                body += filler_line;
            }
            const std::size_t mid = body.size() / 2;
            body.insert(mid, "【配置变更】配置项 mini.gate 已由 白鹭-7600 变更为 白鹭-8600,以本条为准。\n");
            new_long_seg.text = body;
        }
        mini.segments = {long_seg, short_seg, old_seg, new_seg, old_short_seg, new_long_seg};

        // user 消息:6 段 → 2 个 slot(开工 + 段 4 前续读)。contract recall
        // 与 conflict 旧约落 slot 0,conflict 新约落 slot 1。
        UserTurnDef open_turn;
        open_turn.slot = 0;
        open_turn.text = "任务:通读 docs/mini/ 下的工程文档,整理关键配置项与变更记录。\n"
                         "【用户约定】配置项 mini.style 的取值,由用户定死为 承影-4400,全程照此执行,不得偏离。\n"
                         "【用户约定】配置项 mini.log 的取值,用户起初定为 白鹭-7700。\n";
        UserTurnDef next_turn;
        next_turn.slot = 1;
        next_turn.text = "继续,留意配置项的变更记录。\n"
                         "【用户约定变更】配置项 mini.log 的取值,用户已从 白鹭-7700 改定为 白鹭-8700,以本条约定为准,先前约定作废。\n";
        mini.user_turns = {open_turn, next_turn};

        auto make_needle = [](const char* id, const char* layer, const char* kind,
                              const char* carrier, const char* lang, const char* key,
                              std::string expected, std::string old_value, int seg_index,
                              int old_seg_index, int slot, int old_slot) {
            NeedleDef def;
            def.fact_id = id;
            def.layer = layer;
            def.probe_kind = kind;
            def.carrier = carrier;
            def.lang = lang;
            def.key = key;
            def.question = std::string("问 ") + key + "?";
            def.expected_value = std::move(expected);
            def.old_value = std::move(old_value);
            def.seg_index = seg_index;
            def.old_seg_index = old_seg_index;
            def.slot = slot;
            def.old_slot = old_slot;
            return def;
        };
        NeedleDef long_needle = make_needle("MINI-L", "evidence", "recall", "tool_result", "zh",
                                            "mini.long", "青梧-9001", "", 0, -1, -1, -1);
        NeedleDef short_needle = make_needle("MINI-S", "evidence", "recall", "tool_result", "zh",
                                             "mini.short", "青梧-9002", "", 1, -1, -1, -1);
        NeedleDef conflict_a = make_needle("MINI-CA", "evidence", "conflict", "tool_result", "zh",
                                           "mini.port", "玄序-6100", "玄序-3100", 3, 2, -1, -1);
        NeedleDef conflict_b = make_needle("MINI-CB", "evidence", "conflict", "tool_result", "zh",
                                           "mini.gate", "白鹭-8600", "白鹭-7600", 5, 4, -1, -1);
        NeedleDef contract_recall = make_needle("MINI-KR", "contract", "recall", "user_turn", "zh",
                                                "mini.style", "承影-4400", "", -1, -1, 0, -1);
        NeedleDef contract_conflict = make_needle("MINI-KC", "contract", "conflict", "user_turn",
                                                  "zh", "mini.log", "白鹭-8700", "白鹭-7700",
                                                  -1, -1, 1, 0);
        mini.needles = {long_needle, short_needle, conflict_a, conflict_b, contract_recall,
                        contract_conflict};

        // 一臂问答:视图 → grounded 作答 → 判卷,逐 needle 回 verdict。
        const auto grade_arm = [&](const TreatmentOutput& output,
                                   const NeedleDef& needle) -> AnswerVerdict {
            const std::string normalized_view = NormalizeForMatch(ViewText(output.view));
            const ViewPresence presence = CheckViewPresence(normalized_view, needle);
            return GradeAnswer(NormalizeForMatch(AskGroundedFake(needle, presence)), needle);
        };
        // 诊断列(子串在场)自检:与问答口径分开。
        const auto presence_of = [&](const TreatmentOutput& output,
                                     const NeedleDef& needle) -> ViewPresence {
            return CheckViewPresence(NormalizeForMatch(ViewText(output.view)), needle);
        };

        const std::vector<Message> history = BuildHistory(mini, nullptr);

        // FULL:零处理,grounded 满分问答必须六案全 hit(完整性锚)。
        const TreatmentOutput full = ApplyTreatment("FULL", history);
        if (!full.ok || full.view.size() != history.size()) {
            return fail("微型管道 FULL 失败");
        }
        for (const NeedleDef& needle : mini.needles) {
            const AnswerVerdict verdict = grade_arm(full, needle);
            if (!verdict.hit) {
                return fail(std::string("微型管道 FULL 应六案全 hit(装置完整性锚): ") +
                            needle.fact_id + " -> " + verdict.verdict);
            }
        }

        // microcompact:长段 needle 折丢 → grounded 答不出 → lost;短段 → hit;
        // conflict A(新版 medium 全文)→ hit + NewVersion;conflict B(新值
        // 折丢旧值活)→ grounded 答旧值 → stale;contract needle(user 消息
        // 不折叠)→ hit。三类分型在管道里齐现,负路径(stale/lost)真红。
        const TreatmentOutput folded = ApplyTreatment("microcompact", history);
        if (!folded.ok) {
            return fail("微型管道 microcompact 失败: " + folded.error);
        }
        if (folded.offloaded_results == 0) {
            return fail("微型管道 microcompact 没折任何长结果(folding 路没吃到底稿)");
        }
        if (folded.view_bytes >= folded.original_bytes) {
            return fail("微型管道 microcompact 折完没变小");
        }
        if (grade_arm(folded, long_needle).hit ||
            std::string(grade_arm(folded, long_needle).verdict) != "lost") {
            return fail("长段中段 needle 折丢后,grounded 问答应判 lost");
        }
        if (!grade_arm(folded, short_needle).hit) {
            return fail("短段 needle 在折叠视图里问答应判 hit");
        }
        if (!grade_arm(folded, conflict_a).hit) {
            return fail("同 path 新版声明(medium)折叠后问答应判 hit");
        }
        const AnswerVerdict cb = grade_arm(folded, conflict_b);
        if (cb.hit || std::string(cb.verdict) != "stale" || !cb.answer_has_old) {
            return fail("conflict 新值折丢旧值在场:grounded 应答旧值、判卷 stale(stale 陷阱)");
        }
        if (!presence_of(folded, conflict_b).old_in_view ||
            presence_of(folded, conflict_b).new_in_view) {
            return fail("conflict B 的视图诊断列与问答判定矛盾(旧应在场、新不在)");
        }
        if (!grade_arm(folded, contract_recall).hit || !grade_arm(folded, contract_conflict).hit) {
            return fail("contract needle 落 user 消息,折叠后问答应全 hit");
        }
        if (ViewText(folded.view).find("此读取替代事件") == std::string::npos) {
            return fail("同 path 新旧读取没触发折叠路的 NewVersion 分支");
        }

        // compact 假后端路:微型史也要能过四道验收并出 archive+热区;问答
        // 判卷照常跑(假摘要视图,装置只证管道)。
        const TreatmentOutput compacted = ApplyTreatment("compact", history);
        if (!compacted.ok) {
            return fail("微型管道 compact 失败: " + compacted.error);
        }
        if (!compacted.summary_fake || compacted.kept_messages == 0 ||
            compacted.kept_messages > history.size()) {
            return fail("微型管道 compact 热区账不对");
        }
        if (ViewText(compacted.view).find("## 任务目标") == std::string::npos) {
            return fail("compact 视图里应并入假六栏存档");
        }
        if (compacted.budget_window_known) {
            return fail("compact 预算窗口装置阶段应为未知(nullopt,生产同形)");
        }
        for (const NeedleDef& needle : mini.needles) {
            // 只要求判卷器对每案出 verdict(管道通);热区保留是真实行为,
            // 命中多少不设断言(假摘要,语义待真跑)。
            (void)grade_arm(compacted, needle);
        }

        // 确定性:同史两次折叠逐字节一致。
        const TreatmentOutput again = ApplyTreatment("microcompact", history);
        if (ViewText(again.view) != ViewText(folded.view)) {
            return fail("microcompact 同史两次折叠不一致(确定性破)");
        }
    }
    return true;
}

// ---- 主流程 -----------------------------------------------------------------

struct DriverPaths {
    fs::path drafts;
    fs::path results;
    fs::path views;
};

std::vector<fs::path> ListDrafts(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        if (!it->is_regular_file(ec) || it->path().extension() != ".json") {
            continue;
        }
        out.push_back(it->path());
    }
    std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) {
        return PathToUtf8(a) < PathToUtf8(b);
    });
    return out;
}

void DumpView(const fs::path& path, const std::vector<Message>& view) {
    std::ofstream out(path, std::ios::binary);
    for (const auto& message : view) {
        out << (message.role == lubancode::api::Role::User ? "[user] " : "[assistant] ");
        for (const auto& block : message.content) {
            if (const auto* text = std::get_if<lubancode::api::TextBlock>(&block)) {
                out << "text: " << text->text << "\n";
            } else if (const auto* use = std::get_if<lubancode::api::ToolUseBlock>(&block)) {
                out << "tool_use " << use->name << " " << use->input.dump() << "\n";
            } else if (const auto* result = std::get_if<lubancode::api::ToolResultBlock>(&block)) {
                out << "tool_result: " << result->content << "\n";
            }
        }
    }
}

int RunPipeline(const DriverPaths& paths, const std::string& commit) {
    const std::vector<fs::path> draft_files = ListDrafts(paths.drafts);
    if (draft_files.empty()) {
        std::fprintf(stderr, "eval_compact_position: %s 里没有底稿(先跑 generate.py)\n",
                     PathToUtf8(paths.drafts).c_str());
        return 2;
    }

    const fs::path raw_path = paths.results / "raw_position_probe.jsonl";
    const fs::path stats_path = paths.results / "treatment_stats.jsonl";
    std::ofstream raw(raw_path, std::ios::binary);
    std::ofstream stats_out(stats_path, std::ios::binary);
    if (!raw.is_open() || !stats_out.is_open()) {
        std::fprintf(stderr, "eval_compact_position: 原始账写不开(%s)\n",
                     PathToUtf8(paths.results).c_str());
        return 2;
    }

    const char* treatments[] = {"FULL", "microcompact", "compact"};
    const std::string now = NowIso8601Utc();
    std::size_t rows = 0;
    std::size_t full_misses = 0;
    std::size_t microcompact_lost_long = 0;
    std::size_t stale_count = 0;
    std::size_t lost_count = 0;
    bool folding_engaged = false;

    for (const fs::path& draft_file : draft_files) {
        Draft draft;
        try {
            draft = LoadDraft(draft_file);
        } catch (const std::exception& exc) {
            std::fprintf(stderr, "eval_compact_position: %s\n", exc.what());
            return 2;
        }
        HistoryLayout layout;
        const std::vector<Message> history = BuildHistory(draft, &layout);
        const std::size_t original_tokens = lubancode::agent::EstimateHistoryTokens(history);

        for (const char* treatment : treatments) {
            const TreatmentOutput output = ApplyTreatment(treatment, history);
            if (!output.ok) {
                std::fprintf(stderr, "eval_compact_position: %s %s: %s\n", draft.draft_id.c_str(),
                             treatment, output.error.c_str());
                return 1;  // 装置坏,不是判卷 miss
            }
            if (treatment == std::string("microcompact") && output.view_bytes < output.original_bytes) {
                folding_engaged = true;
            }

            const std::string view_text = ViewText(output.view);
            const std::string normalized_view = NormalizeForMatch(view_text);
            std::set<std::size_t> kept(output.kept_indices.begin(), output.kept_indices.end());

            for (const NeedleDef& needle : draft.needles) {
                // 问答判卷(铁律 1):grounded 假后端读视图作答 → 判卷比对回答。
                const ViewPresence presence = CheckViewPresence(normalized_view, needle);
                const std::string model_answer = AskGroundedFake(needle, presence);
                const AnswerVerdict verdict =
                    GradeAnswer(NormalizeForMatch(model_answer), needle);
                if (verdict.verdict == std::string("stale")) {
                    ++stale_count;
                }
                if (verdict.verdict == std::string("lost")) {
                    ++lost_count;
                }
                if (std::string(treatment) == "FULL" && !verdict.hit) {
                    ++full_misses;  // FULL 全 hit 是装置完整性锚,循环后断言
                }
                if (std::string(treatment) == "microcompact" && !verdict.hit &&
                    needle.seg_length_class == "long") {
                    ++microcompact_lost_long;
                }
                // hot_kept:evidence 看落段消息,contract 看落 user 消息。
                json hot_kept = nullptr;
                if (!output.kept_indices.empty()) {
                    const std::size_t message_index =
                        needle.carrier == "user_turn"
                            ? (needle.slot >= 0 &&
                                       static_cast<std::size_t>(needle.slot) <
                                           layout.user_message_of_slot.size()
                                   ? layout.user_message_of_slot[static_cast<std::size_t>(needle.slot)]
                                   : 0)
                            : (needle.seg_index >= 0 &&
                                       static_cast<std::size_t>(needle.seg_index) <
                                           layout.result_message_of_segment.size()
                                   ? layout.result_message_of_segment[static_cast<std::size_t>(
                                         needle.seg_index)]
                                   : 0);
                    hot_kept = json(kept.count(message_index) > 0);
                }
                json record;
                record["experiment"] = kExperiment;
                record["draft_id"] = draft.draft_id;
                record["lang"] = draft.lang;
                record["repeat"] = draft.repeat;
                record["seed"] = draft.seed;
                record["treatment"] = treatment;
                record["layer"] = needle.layer;
                record["probe_kind"] = needle.probe_kind;
                record["carrier"] = needle.carrier;
                record["fact_id"] = needle.fact_id;
                record["key"] = needle.key;
                record["position_pct"] = needle.position_pct;
                record["actual_position_pct"] = needle.actual_position_pct;
                record["seg_index"] = needle.seg_index >= 0 ? json(needle.seg_index) : json(nullptr);
                record["seg_length_class"] = needle.seg_length_class.empty()
                                                 ? json(nullptr)
                                                 : json(needle.seg_length_class);
                record["slot"] = needle.slot >= 0 ? json(needle.slot) : json(nullptr);
                record["offset_pct_in_seg"] = needle.offset_pct_in_seg;
                record["expected_value"] = needle.expected_value;
                record["old_value"] = needle.old_value.empty() ? json(nullptr) : json(needle.old_value);
                // 问答判卷三件:题面、模型回答、判定。
                record["question"] = needle.question;
                record["model_answer"] = model_answer;
                record["verdict"] = verdict.verdict;
                record["hit"] = verdict.hit;
                record["answer_has_new"] = verdict.answer_has_new;
                record["answer_has_old"] = verdict.answer_has_old;
                // 视图级子串在场:辅助诊断列(FULL/microcompact 机械性信号;
                // compact 臂不据此记成绩)。
                record["new_in_view"] = presence.new_in_view;
                record["old_in_view"] = presence.old_in_view;
                record["hot_kept"] = hot_kept;
                record["view_bytes"] = output.view_bytes;
                record["original_bytes"] = output.original_bytes;
                record["summary_fake"] = output.summary_fake;
                record["answering_model"] = kAnsweringModel;
                record["commit"] = commit;
                record["model"] = kFakeModel;
                record["provider"] = kFakeProvider;
                record["recorded_at"] = now;
                raw << record.dump() << "\n";
                ++rows;
            }

            json stat;
            stat["experiment"] = kExperiment;
            stat["draft_id"] = draft.draft_id;
            stat["lang"] = draft.lang;
            stat["repeat"] = draft.repeat;
            stat["seed"] = draft.seed;
            stat["treatment"] = treatment;
            stat["original_bytes"] = output.original_bytes;
            stat["view_bytes"] = output.view_bytes;
            stat["original_tokens"] = original_tokens;
            stat["view_tokens"] = lubancode::agent::EstimateHistoryTokens(output.view);
            stat["messages_total"] = layout.message_count;
            stat["messages_kept"] = output.kept_messages;
            stat["offloaded_results"] = output.offloaded_results;
            stat["duplicate_groups"] = output.duplicate_groups;
            stat["superseded_observations"] = output.superseded_observations;
            stat["offloaded_saved_bytes"] = output.offloaded_saved_bytes;
            stat["compact_requests"] = output.compact_requests;
            stat["summary_fake"] = output.summary_fake;
            // compact 预算形态如实记(生产默认:窗口未知,nullopt 不做拦截)。
            stat["budget_window_tokens"] =
                output.budget_window_known ? json(output.budget_window_tokens) : json(nullptr);
            stat["answering_model"] = kAnsweringModel;
            stat["commit"] = commit;
            stat["model"] = kFakeModel;
            stat["provider"] = kFakeProvider;
            stat["recorded_at"] = now;
            stats_out << stat.dump() << "\n";

            DumpView(paths.views / (draft.draft_id + "." + treatment + ".txt"), output.view);
        }
    }

    // ---- 装置完整性断言(不是判卷账) ----
    if (full_misses != 0) {
        std::fprintf(stderr,
                     "eval_compact_position: FULL 问答判失 %zu 案——FULL 是零处理基线,"
                     "grounded 满分模型答不对即装置坏(判卷/造稿/视图投影出了错)\n",
                     full_misses);
        return 1;
    }
    if (!folding_engaged) {
        std::fprintf(stderr,
                     "eval_compact_position: microcompact 折叠路全程没压小任何一份视图——"
                     "折叠没吃到底稿,装置坏\n");
        return 1;
    }
    if (microcompact_lost_long == 0) {
        std::fprintf(stderr,
                     "eval_compact_position: microcompact 没折掉任何 long 档 needle——"
                     "首笔真语义信号缺位,查造稿长度分布\n");
        return 1;
    }
    if (stale_count == 0 || lost_count == 0) {
        std::fprintf(stderr,
                     "eval_compact_position: 问答判卷三型未齐现(stale=%zu lost=%zu)——"
                     "失败注入没在管道里真红过,判卷器或 grounded 作答器退化\n",
                     stale_count, lost_count);
        return 1;
    }

    std::printf("eval_compact_position: %zu 份底稿 x 3 处理 -> %zu 案落 %s\n",
                draft_files.size(), rows, PathToUtf8(raw_path).c_str());
    std::printf("eval_compact_position: 问答判卷三型齐现——hit %zu 案,stale %zu 案,lost %zu 案"
                "(microcompact 折丢 long 档 needle %zu 案;负路径主链实证)\n",
                rows - stale_count - lost_count, stale_count, lost_count, microcompact_lost_long);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    fs::path drafts;
    fs::path results;
    bool self_check_only = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--self-check") {
            self_check_only = true;
        } else if (arg == "--drafts" && i + 1 < argc) {
            drafts = fs::path(argv[++i]);
        } else if (arg == "--results" && i + 1 < argc) {
            results = fs::path(argv[++i]);
        } else if (arg == "--real") {
            std::fprintf(stderr,
                         "eval_compact_position: --real 真跑路未接线(装置阶段零真请求、"
                         "零真钥匙);真跑三条命令见 README,接线时假后端替身换真后端工厂、"
                         "问答方换实验模型 ccmoon/gpt-5.6-luna\n");
            return 2;
        } else {
            std::fprintf(stderr, "用法: eval_compact_position [--drafts <dir>] [--results <dir>] "
                                 "[--self-check]\n");
            return 2;
        }
    }
#ifdef LUBANCODE_EVAL_COMPACT_POSITION_ROOT
    const fs::path root = PathToUtf8(drafts).empty()
                              ? fs::path(LUBANCODE_EVAL_COMPACT_POSITION_ROOT)
                              : fs::path();
#else
    const fs::path root;
#endif
    if (PathToUtf8(drafts).empty()) {
        drafts = root / "results" / "drafts";
    }
    if (PathToUtf8(results).empty()) {
        results = drafts.parent_path().empty() ? fs::path("results") : drafts.parent_path();
    }

    std::string error;
    if (!RunSelfChecks(&error)) {
        std::fprintf(stderr, "eval_compact_position: 自检不过: %s\n", error.c_str());
        return 1;
    }
    if (self_check_only) {
        std::printf("eval_compact_position: 自检全过(归一化/问答判卷+失败注入/grounded 作答/"
                    "微型管道/确定性)\n");
        return 0;
    }
    if (PathToUtf8(root).empty() && PathToUtf8(drafts).empty()) {
        std::fprintf(stderr, "eval_compact_position: 缺底稿目录(参数或编译期根)\n");
        return 2;
    }

    DriverPaths paths;
    paths.drafts = drafts;
    paths.results = results;
    paths.views = results / "views";
    std::error_code ec;
    fs::create_directories(paths.results, ec);
    fs::create_directories(paths.views, ec);
    if (ec) {
        std::fprintf(stderr, "建不了输出目录 %s: %s\n", PathToUtf8(paths.results).c_str(),
                     ec.message().c_str());
        return 2;
    }

    const std::string commit =
#ifdef LUBANCODE_GIT_COMMIT
        LUBANCODE_GIT_COMMIT;
#else
        "unknown";
#endif
    return RunPipeline(paths, commit);
}
