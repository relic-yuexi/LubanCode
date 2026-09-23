// TUI 排版批 5a(tui优化todo.todo P5:/session·/context)的输出形状册。
//   - 旧"── 组名 ──"手拼横线收口:缓存/结构与回收/预算与角色账三组各进
//     键值对框,组名嵌框顶标题;占用卡片(cli 层 FormatContextBreakdown,
//     批 5a 领地外)原样,卡片尾部垫 divider::line(测试进程探不到终端宽,
//     线不画——真机宽度由主人会话内连敲验收);
//   - 组内句子按 SentenceField 拆键值对(批 1 裁量 2:拆的是既有文案);
//   - 口径说明 + 校准行进无标题键值对框;
//   - plain 主题(--no-color/T3 降级路径)钉零转义字节、无框字形——合同
//     第 3 条;
//   - /context 无 --json/机器消费分支(源码核实:json 命中全是 nlohmann
//     数据处理与提示词模板,无命令输出侧机器面),字节级不变一条天然满足。
//
// 走真 HandleContextCommand(手造 ContextTracker + TerminalPort 改道捕获),
// 断言只看形状与相对位置,不看绝对宽度。

#include <doctest/doctest.h>

#include <sstream>
#include <string>

#include "api/types.hpp"
#include "agent/context_budget.hpp"  // ContextBudgetPlan:预算总账材料
#include "app/commands/session_commands.hpp"
#include "agent/runtime_profile.hpp"
#include "cli/context_tracker.hpp"
#include "cli/format_utils.hpp"  // FormatTokenCount:期望串与实现同一把尺
#include "cli/line_editor.hpp"   // DisplayWidthUtf8:对齐断言按显示列量
#include "cli/terminal_port.hpp"
#include "cli/theme.hpp"

using namespace lubancode;

namespace {

class OutputCapture {
public:
    OutputCapture() { cli::TermPort().Redirect(&stream_, nullptr); }
    ~OutputCapture() { cli::TermPort().Reset(); }
    std::string text() const { return stream_.str(); }

private:
    std::ostringstream stream_;
};

std::string StripAnsi(const std::string& text) {
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[') {
            i += 2;
            while (i < text.size() &&
                   !((text[i] >= 'a' && text[i] <= 'z') || (text[i] >= 'A' && text[i] <= 'Z'))) {
                ++i;
            }
            if (i < text.size()) {
                ++i;  // 吃掉终结字母
            }
            continue;
        }
        out += text[i];
        ++i;
    }
    return out;
}

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

int DisplayColOf(const std::string& row, const std::string& needle) {
    const std::size_t at = row.find(needle);
    if (at == std::string::npos) {
        return -1;
    }
    return static_cast<int>(cli::DisplayWidthUtf8(row.substr(0, at)));
}

constexpr const char* kBoxLightTopLeft = "\xe2\x94\x8c";  // ┌
constexpr const char* kBoxLightVert = "\xe2\x94\x82";     // │

const cli::Theme dark = cli::BuiltinTheme("dark");
const cli::Theme plain = cli::BuiltinTheme("plain");

// 一场发过请求、有缓存账的 v2 会话材料:三层卡全出。
std::string RunContext(const cli::Theme& theme, const app::ContextLayersReport* layers,
                       const agent::AgentRuntimeProfile* profile) {
    cli::ContextTracker tracker(200000);
    api::Usage usage;
    usage.input_tokens = 40000;
    usage.cache_read_tokens = 60000;
    usage.output_tokens = 800;
    tracker.ApplyUsage(usage, /*turn_id=*/"t1", /*step_index=*/1);
    app::ContextSessionFacts facts;  // v2:全默认
    runtime::PreRequestBudget budget;
    budget.context_window_tokens = 200000;
    budget.declared_max_output_tokens = 8192;
    budget.policy_reserve_tokens = 8192;
    budget.final_reserve_tokens = 8192;
    budget.effective_output_limit_tokens = 8192;
    facts.last_request_budget = &budget;
    OutputCapture capture;
    app::HandleContextCommand("", tracker, 100, 200, 300, theme, /*cache_epoch=*/2, profile,
                              /*usage_ledger=*/nullptr, layers,
                              /*roles_table=*/nullptr, /*compact_partition_count=*/3,
                              /*deferred_tool_summary=*/nullptr, /*token_calibration=*/nullptr,
                              facts);
    return capture.text();
}

// 带 v2 预算计划的分层材料(预算总账/开销明细/压缩预算出自这里)。
app::ContextLayersReport MakeLayersWithBudget() {
    app::ContextLayersReport layers;
    layers.inline_full_results = 4;
    layers.artifact_previews = 2;
    layers.reclaimable_bytes = 4096;
    layers.last_compact_line = "cheap:m · 62k→18k · 3.2s · 校验通过";
    agent::ContextBudgetPlan plan;
    plan.window = 200000;
    plan.compactable_history_budget = 100000;
    plan.stable_system = 4000;
    plan.tool_schemas = 6000;
    plan.protected_hot_zone = 8000;
    plan.requested_output_reserve = 8192;
    plan.compact_prompt_overhead = 4000;
    plan.protocol_headroom = 1000;
    plan.tokenizer_error_margin = 2000;
    plan.compact_call_input_budget = 50000;
    layers.budget = plan;
    return layers;
}

}  // namespace

TEST_CASE("context:三组各进键值对框,组名嵌框顶标题") {
    const app::ContextLayersReport layers = MakeLayersWithBudget();
    agent::AgentRuntimeProfile profile;

    const std::string out = RunContext(dark, &layers, &profile);
    const std::string text = StripAnsi(out);

    // 占用卡片(cli 层)在前——排版批 6 起表头也收口:组名进标题
    // (frame_title 色,无框卡片就一行标题文字),旧"── 占用 ──"手拼横线
    // 绝迹;三组框标题跟后。
    REQUIRE(Contains(out, kBoxLightTopLeft));
    CHECK(Contains(text, "占用(窗口"));
    CHECK(text.find("── 占用 ──") == std::string::npos);
    CHECK(Contains(text, "缓存"));
    CHECK(Contains(text, "结构与回收"));
    CHECK(Contains(text, "预算与角色账"));
    // 旧"── 组名 ──"手拼横线不再出现(批 5a 收口命令层三处组名横线,
    // 批 6 把占用卡片表头也收进标题——全输出再无手拼组名横线)。
    CHECK(text.find("── 缓存 ──") == std::string::npos);
    CHECK(text.find("── 结构与回收 ──") == std::string::npos);
    CHECK(text.find("── 预算与角色账 ──") == std::string::npos);

    // 组内句子拆键值对:键与值都在,信息一字不丢。
    CHECK(Contains(text, "前缀 epoch 2"));
    CHECK(Contains(text, "命中"));
    CHECK(Contains(text, "会话累计"));
    CHECK(Contains(text, "分层占用"));
    CHECK(Contains(text, "预算总账"));
    CHECK(Contains(text, "最近请求预算"));
    CHECK(Contains(text, "开销明细"));
    CHECK(Contains(text, "压缩预算"));
    CHECK_FALSE(Contains(text, "口径说明"));  // 口径小节无组名,无标题
    CHECK(Contains(text, "不是累计花销"));     // note.semantics 的正文仍在
}

TEST_CASE("context:键值对按最宽 key 对齐,value 起始列处处一致") {
    const app::ContextLayersReport layers = MakeLayersWithBudget();
    agent::AgentRuntimeProfile profile;
    const std::string out = RunContext(dark, &layers, &profile);
    const std::string text = StripAnsi(out);

    // 缓存组:epoch 行与会话累计行的 value 同列(RenderKeyValues 按全组最
    // 宽 key 补齐)。行选与探针共守:note.semantics 长句里也有"会话累计"
    // 字样,单按行选会截胡(不含"命中 60.0k"则不是缓存组的行)。
    const std::string hit_head = "命中 " + cli::FormatTokenCount(60000);
    int epoch_col = -1;
    int session_col = -1;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (Contains(line, "前缀 epoch 2") && Contains(line, hit_head)) {
            epoch_col = DisplayColOf(line, hit_head);
        }
        if (Contains(line, "会话累计") && Contains(line, hit_head)) {
            session_col = DisplayColOf(line, hit_head);
        }
    }
    CHECK(epoch_col > 0);
    CHECK(session_col > 0);
    CHECK(epoch_col == session_col);
    CHECK(Contains(text, hit_head));
}

TEST_CASE("context:键列走 row_label 语义色,占用卡片在前组框在后") {
    const app::ContextLayersReport layers = MakeLayersWithBudget();
    agent::AgentRuntimeProfile profile;
    const std::string out = RunContext(dark, &layers, &profile);
    // 键列色:row_label 包着"前缀 epoch 2"与"预算总账"。
    CHECK(Contains(out, dark.row_label + "前缀 epoch 2"));
    CHECK(Contains(out, dark.row_label + "预算总账"));
    // 顺序:占用卡片表头在三组框标题之前。
    const std::string text = StripAnsi(out);
    CHECK(text.find("── 占用 ──") < text.find("缓存"));
    CHECK(text.find("缓存") < text.find("结构与回收"));
}

TEST_CASE("context:v3 会话口径行照出(v3_estimator/结果仓/结构压缩层)") {
    cli::ContextTracker tracker(100000);
    app::ContextSessionFacts facts;
    facts.v3_session = true;
    facts.has_result_store_stats = true;
    facts.result_store_results = 3;
    facts.result_store_bytes = 1048576;
    OutputCapture capture;
    app::HandleContextCommand("", tracker, 100, 200, 300, dark, /*cache_epoch=*/1,
                              /*main_profile=*/nullptr, /*usage_ledger=*/nullptr,
                              /*layers=*/nullptr, /*roles_table=*/nullptr,
                              /*compact_partition_count=*/0, /*deferred_tool_summary=*/nullptr,
                              /*token_calibration=*/nullptr, facts);
    const std::string text = StripAnsi(capture.text());
    CHECK(Contains(text, "utf8_bytes_div4"));
    CHECK(Contains(text, "结果仓"));
    CHECK(Contains(text, "结构压缩层"));
}

TEST_CASE("plain 主题:全输出零转义字节、无框字形(T3/--no-color 路径)") {
    const app::ContextLayersReport layers = MakeLayersWithBudget();
    agent::AgentRuntimeProfile profile;
    const std::string out = RunContext(plain, &layers, &profile);
    CHECK(out.find("\x1b") == std::string::npos);
    CHECK(!Contains(out, kBoxLightTopLeft));
    CHECK(!Contains(out, kBoxLightVert));
    // 信息一字不少:组名与句子都在,只是没了色与框。
    CHECK(Contains(out, "缓存"));
    CHECK(Contains(out, "结构与回收"));
    CHECK(Contains(out, "预算与角色账"));
    CHECK(Contains(out, "前缀 epoch 2"));
    CHECK(Contains(out, "预算总账"));
}
