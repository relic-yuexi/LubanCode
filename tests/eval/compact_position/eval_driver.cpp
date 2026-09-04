// 实验 B1(compact 位置探针,Q2 量化评测单 §二 B1 首仗)的三处理驱动:
// 同一份底稿(造稿器 generate.py 落 results/drafts/),三种处理各出一版
// "请求视图",对视图做确定性判卷,逐 needle 落一行 JSONL 原始账。
//
// 三处理(单子 §二 B1):
//   FULL         原文直发(零处理,对照基线)。视图判卷阶段 FULL 应全存活
//                ——U 形是模型现象,留给真模型问答阶段;这里它是装置的
//                完整性锚(任一 recall 判失即装置坏了,exit 1)。
//   microcompact 喂产品的无损结构压缩折叠路 agent::CompressWorkingView
//                (in-process,零模型,默认参数 long_result_bytes=8192/
//                preview_bytes=256)。超长 read 结果被换成头尾 256B 预览,
//                落在段中段的 needle 真丢——这是本装置第一笔真语义信号,
//                与位置档无关、与段内深度有关,分桶表(fold_survival)记账。
//                冲突类 needle 的新旧两版刻意同 path 再读(同键不同 hash),
//                正好踩折叠路的"文件改版"(NewVersion)分支。
//   compact      喂六栏摘要路 agent::Compact + BuildCompactedHistory。
//                真模型才跑得动,装置阶段用 FakeStreamingBackend 回一份
//                固定形态的六栏摘要 + JSON manifest 替身——摘要内容是假的,
//                只验管道(采样→三道验收→archive→热区保留);语义待真跑,
//                账里 summary_fake=true 如实标注。热区保留是真实行为
//                (kDefaultHotZoneTokens=12000),needle 存活只可能来自热区。
//
// 判卷(确定性,独立自检):
//   归一化(ASCII 小写、剥空白/全角空格/零宽)后做子串匹配。
//   recall: 期望值在视图 → hit,否则 lost。
//   conflict(更新冲突): 新值在 → hit;新值不在而旧值在 → stale
//            (旧值已 superseded,拿它作答就是错——"不作答也不算对");
//            两值都不在 → lost。
//
// 用法:
//   eval_compact_position [--drafts <dir>] [--results <dir>] [--self-check]
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

struct NeedleDef {
    std::string fact_id;
    std::string probe_kind;  // recall | conflict
    std::string lang;
    int position_pct = 0;
    double actual_position_pct = 0.0;
    int seg_index = 0;
    int old_seg_index = -1;
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
    for (const auto& needle : root.at("needles")) {
        NeedleDef def;
        def.fact_id = StringOr(needle, "fact_id");
        def.probe_kind = StringOr(needle, "probe_kind");
        def.lang = StringOr(needle, "lang");
        def.position_pct = IntOr(needle, "position_pct", 0);
        def.actual_position_pct = DoubleOr(needle, "actual_position_pct", 0.0);
        def.seg_index = IntOr(needle, "seg_index", 0);
        def.old_seg_index = IntOr(needle, "old_seg_index", -1);
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
// 才有区分度;20 段一大 turn 时热区只剩末轮问话,compact 臂全线零存活,
// 量不出"曲线压成什么样")→ assistant 收口 → user 终问(热区锚点)。
// tool_use/tool_result 紧邻配对,与 BuildEventLedger 的原子配对形状一致。

struct HistoryLayout {
    // 每段 read 结果所在的 user 消息下标(hot_kept 判定用)。
    std::vector<std::size_t> result_message_of_segment;
    std::size_t message_count = 0;
};

std::vector<Message> BuildHistory(const Draft& draft, HistoryLayout* layout) {
    std::vector<Message> history;
    history.reserve(draft.segments.size() * 2 + 8);
    const bool zh = draft.lang == "zh";

    Message open;
    open.role = lubancode::api::Role::User;
    open.content.push_back(lubancode::api::TextBlock{
        zh ? "任务:通读 docs/draft/ 下的工程文档,整理其中的关键配置项与变更记录。"
           : "Task: read through the engineering docs under docs/draft/ and collect key settings and changes."});
    history.push_back(std::move(open));

    constexpr std::size_t kBatch = 4;
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
            next.content.push_back(lubancode::api::TextBlock{
                zh ? "继续,留意配置项的变更记录。" : "Continue; watch for setting changes."});
            history.push_back(std::move(next));
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
// 不进——事实值只出现在结果正文里)。
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

// ---- 判卷(确定性) -----------------------------------------------------------

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

// 判卷结果:verdict ∈ hit | stale | lost。
//   recall:   期望值在 → hit;不在 → lost。
//   conflict: 新值在 → hit;新值不在而旧值在 → stale(superseded 旧值不得
//             作答,装置口径:拿旧值答与答不出同记 miss,stale 单列);
//             两值都不在 → lost。
struct NeedleVerdict {
    bool hit = false;
    bool new_present = false;
    bool old_present = false;
    const char* verdict = "lost";
};

NeedleVerdict GradeNeedle(const std::string& normalized_view, const NeedleDef& needle) {
    NeedleVerdict out;
    out.new_present = ContainsValue(normalized_view, needle.expected_value);
    out.old_present = !needle.old_value.empty() && ContainsValue(normalized_view, needle.old_value);
    if (needle.probe_kind == "conflict") {
        if (out.new_present) {
            out.hit = true;
            out.verdict = "hit";
        } else if (out.old_present) {
            out.verdict = "stale";  // 旧值活着、新值没了:照旧值答就是错
        }
        return out;
    }
    out.hit = out.new_present;
    out.verdict = out.new_present ? "hit" : "lost";
    return out;
}

// ---- 三处理 -----------------------------------------------------------------

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
    // compact 管道账
    bool summary_fake = false;
    std::size_t compact_requests = 0;
    std::size_t kept_messages = 0;
    std::vector<std::size_t> kept_indices;
    bool ok = false;
    std::string error;
};

// compact 假后端替身:固定形态的六栏摘要 + 末尾 ```json manifest。内容是假
// 的(不取自底稿),只保证过 Compact 的三道验收(≥40 码点、manifest 可解析、
// goal 非空)与第四道(历史比摘要大)。语义待真模型跑。
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
        lubancode::agent::CompactOptions options;  // window_tokens=nullopt:装置未按窗口校验
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

// ---- 自检(判卷与管道的单测,eval 冒烟章法:进程内断言) ----------------------

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

    // 2) recall 判卷:在 → hit;不在 → lost。
    {
        NeedleDef needle;
        needle.probe_kind = "recall";
        needle.expected_value = "青梧-1009";
        NeedleVerdict hit = GradeNeedle(NormalizeForMatch("正文里写着:配置项核定为 青梧-1009,以此为准。"), needle);
        if (!hit.hit || std::string(hit.verdict) != "hit") {
            return fail("recall 应判 hit");
        }
        NeedleVerdict lost = GradeNeedle(NormalizeForMatch("正文里没有事实值。"), needle);
        if (lost.hit || std::string(lost.verdict) != "lost") {
            return fail("recall 应判 lost");
        }
        NeedleDef en_needle;
        en_needle.probe_kind = "recall";
        en_needle.expected_value = "Qingwu-1009";
        NeedleVerdict spaced = GradeNeedle(NormalizeForMatch("value is  QINGWU-1009  today"), en_needle);
        if (!spaced.hit) {
            return fail("recall 值应能匹配(空格与大小写不吃措辞)");
        }
    }

    // 3) conflict 判卷:新值在 → hit;仅旧值在 → stale(不作答也不算对);
    //    两不在 → lost。注意新值声明里也提及旧值——新值在时旧值串在场
    //    属正常,verdict 只看新值。
    {
        NeedleDef needle;
        needle.probe_kind = "conflict";
        needle.expected_value = "玄序-6100";
        needle.old_value = "玄序-3100";
        const NeedleVerdict hit = GradeNeedle(
            NormalizeForMatch("【配置变更】已由 玄序-3100 变更为 玄序-6100,以本条为准。"), needle);
        if (!hit.hit || !hit.new_present || !hit.old_present) {
            return fail("conflict 新值在场应判 hit");
        }
        const NeedleVerdict stale = GradeNeedle(
            NormalizeForMatch("【配置基线】初始设定为 玄序-3100。"), needle);
        if (stale.hit || std::string(stale.verdict) != "stale" || !stale.old_present) {
            return fail("conflict 仅旧值在场应判 stale 且不算 hit");
        }
        const NeedleVerdict lost = GradeNeedle(NormalizeForMatch("别的正文。"), needle);
        if (lost.hit || std::string(lost.verdict) != "lost") {
            return fail("conflict 两值都不在应判 lost");
        }
    }

    // 4) 微型管道:一段超长(needle 居中)+ 一段短(needle 在场)+ 一对同
    //    path 新旧读取。FULL 全存活;microcompact 折掉长段中段 needle、
    //    短段 needle 活、同 path 新版声明触发 NewVersion 分支。
    {
        Draft mini;
        mini.draft_id = "mini";
        mini.lang = "zh";
        SegmentDef long_seg;
        long_seg.tool_use_id = "u_long";
        long_seg.path = "docs/mini/long.md";
        long_seg.length_class = "long";
        {
            std::string filler_line = "构建日志:模块 042 编译完成,警告 3 条,耗时 217 毫秒。\n";
            std::size_t lines = 0;
            long_seg.text.reserve(10 * 1024);
            while (long_seg.text.size() < 10 * 1024) {
                long_seg.text += filler_line;
                ++lines;
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
        const std::string pad_line = "巡检记录:节点 041 的磁盘水位 38%,仍在安全带内。\n";
        for (std::size_t pad = 0; pad < 24; ++pad) {
            old_seg.text += pad_line;
            new_seg.text += pad_line;
        }
        mini.segments = {long_seg, short_seg, old_seg, new_seg};

        NeedleDef long_needle;
        long_needle.fact_id = "MINI-L";
        long_needle.probe_kind = "recall";
        long_needle.expected_value = "青梧-9001";
        long_needle.seg_index = 0;
        NeedleDef short_needle;
        short_needle.fact_id = "MINI-S";
        short_needle.probe_kind = "recall";
        short_needle.expected_value = "青梧-9002";
        short_needle.seg_index = 1;
        NeedleDef conflict_needle;
        conflict_needle.fact_id = "MINI-C";
        conflict_needle.probe_kind = "conflict";
        conflict_needle.expected_value = "玄序-6100";
        conflict_needle.old_value = "玄序-3100";
        conflict_needle.seg_index = 3;
        conflict_needle.old_seg_index = 2;
        mini.needles = {long_needle, short_needle, conflict_needle};

        const std::vector<Message> history = BuildHistory(mini, nullptr);

        const TreatmentOutput full = ApplyTreatment("FULL", history);
        if (!full.ok || full.view.size() != history.size()) {
            return fail("微型管道 FULL 失败");
        }
        const std::string full_text = NormalizeForMatch(ViewText(full.view));
        if (!GradeNeedle(full_text, long_needle).hit || !GradeNeedle(full_text, short_needle).hit ||
            std::string(GradeNeedle(full_text, conflict_needle).verdict) != "hit") {
            return fail("微型管道 FULL 应三案全存活");
        }

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
        const std::string folded_text = NormalizeForMatch(ViewText(folded.view));
        if (GradeNeedle(folded_text, long_needle).hit) {
            return fail("长段中段 needle 在折叠视图里竟然还活着——真语义信号失灵");
        }
        if (!GradeNeedle(folded_text, short_needle).hit) {
            return fail("短段 needle 在折叠视图里不该死");
        }
        const NeedleVerdict conflict_folded = GradeNeedle(folded_text, conflict_needle);
        if (!conflict_folded.hit) {
            return fail("同 path 新版声明(短正文)在折叠视图里应全文在场");
        }
        if (ViewText(folded.view).find("此读取替代事件") == std::string::npos) {
            return fail("同 path 新旧读取没触发折叠路的 NewVersion 分支");
        }

        // compact 假后端路:微型史也要能过四道验收并出 archive+热区。
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
    std::size_t full_recall_misses = 0;
    std::size_t microcompact_lost_long = 0;
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
            const std::string normalized = NormalizeForMatch(view_text);
            std::set<std::size_t> kept(output.kept_indices.begin(), output.kept_indices.end());

            for (const NeedleDef& needle : draft.needles) {
                const NeedleVerdict verdict = GradeNeedle(normalized, needle);
                if (std::string(treatment) == "FULL" && needle.probe_kind == "recall" && !verdict.hit) {
                    ++full_recall_misses;  // FULL 全存活是装置完整性锚,循环后断言
                }
                if (std::string(treatment) == "microcompact" && !verdict.hit &&
                    needle.seg_length_class == "long") {
                    ++microcompact_lost_long;
                }
                json record;
                record["experiment"] = kExperiment;
                record["draft_id"] = draft.draft_id;
                record["lang"] = draft.lang;
                record["repeat"] = draft.repeat;
                record["seed"] = draft.seed;
                record["treatment"] = treatment;
                record["probe_kind"] = needle.probe_kind;
                record["fact_id"] = needle.fact_id;
                record["position_pct"] = needle.position_pct;
                record["actual_position_pct"] = needle.actual_position_pct;
                record["seg_index"] = needle.seg_index;
                record["seg_length_class"] = needle.seg_length_class;
                record["offset_pct_in_seg"] = needle.offset_pct_in_seg;
                record["expected_value"] = needle.expected_value;
                record["old_value"] = needle.old_value.empty() ? json(nullptr) : json(needle.old_value);
                record["hit"] = verdict.hit;
                record["verdict"] = verdict.verdict;
                record["new_present"] = verdict.new_present;
                record["old_present"] = verdict.old_present;
                record["hot_kept"] = output.kept_indices.empty()
                                         ? json(nullptr)
                                         : json(kept.count(layout.result_message_of_segment[
                                                   static_cast<std::size_t>(needle.seg_index)]) > 0);
                record["view_bytes"] = output.view_bytes;
                record["original_bytes"] = output.original_bytes;
                record["summary_fake"] = output.summary_fake;
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
            stat["commit"] = commit;
            stat["model"] = kFakeModel;
            stat["provider"] = kFakeProvider;
            stat["recorded_at"] = now;
            stats_out << stat.dump() << "\n";

            DumpView(paths.views / (draft.draft_id + "." + treatment + ".txt"), output.view);
        }
    }

    // ---- 装置完整性断言(不是判卷账) ----
    if (full_recall_misses != 0) {
        std::fprintf(stderr,
                     "eval_compact_position: FULL 视图里 recall 判失 %zu 案——FULL 是零处理"
                     "基线,判失即装置坏(判卷或造稿出了错)\n",
                     full_recall_misses);
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

    std::printf("eval_compact_position: %zu 份底稿 x 3 处理 -> %zu 案落 %s\n",
                draft_files.size(), rows, PathToUtf8(raw_path).c_str());
    std::printf("eval_compact_position: microcompact 折掉 long 档 needle %zu 案(真语义信号)\n",
                microcompact_lost_long);
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
        std::printf("eval_compact_position: 自检全过(归一化/判卷/微型管道/确定性)\n");
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
