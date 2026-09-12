// /context 卡片的 v3 会话口径(Session v3 真实会话审计单 V3-REAL-03/08/
// A02 的展示面回归):历史无测——/context 一直是纯打印函数。本册用
// TerminalPort::Redirect 捕获输出,钉三件事:
//   1. v3 会话不打 v2 校准行,改打 bytes/4 估算口径行(在线校准不进 v3
//      显示);
//   2. 最近请求预算分栏(声明/策略预留/判定预留/实发),没发过请求就明说
//      ——不拿配置现算冒充历史请求;
//   3. v3 结果仓统计行(结果已保存,当前仍以预览进模型)与"结构压缩层
//      未启用"行;v2 会话照旧走旧 artifact/校准口径,一字不动。
#include <doctest/doctest.h>

#include <sstream>
#include <string>

#include "agent/agent.hpp"
#include "agent/runtime_profile.hpp"
#include "app/commands/session_commands.hpp"
#include "cli/context_tracker.hpp"
#include "cli/terminal_port.hpp"
#include "cli/theme.hpp"

using namespace lubancode;

namespace {

// 捕获 TermOut 输出的守卫:构造重定向,析构还原。
class OutputCapture {
public:
    OutputCapture() { cli::TermPort().Redirect(&buffer_, &buffer_); }
    ~OutputCapture() { cli::TermPort().Reset(); }
    const std::string text() const { return buffer_.str(); }

private:
    std::ostringstream buffer_;
};

app::ContextSessionFacts V3Facts() {
    app::ContextSessionFacts facts;
    facts.v3_session = true;
    facts.has_result_store_stats = true;
    facts.result_store_results = 3;
    facts.result_store_bytes = 1048576;
    return facts;
}

}  // namespace

TEST_CASE("v3 会话:/context 打 bytes/4 口径行,不打 v2 校准行") {
    app::ContextSessionFacts facts = V3Facts();
    cli::ContextTracker tracker(100000);
    const cli::Theme theme;
    agent::TokenCalibrationStatus calibrated;
    calibrated.calibrated = true;
    calibrated.sample_count = 4;
    calibrated.coefficient = 1.59;  // UHURZ3 现场那枚系数,不许再进 v3 显示

    std::string text;
    {
        OutputCapture capture;
        app::HandleContextCommand("", tracker, 100, 200, 300, theme, /*cache_epoch=*/2,
                                  /*main_profile=*/nullptr, /*usage_ledger=*/nullptr,
                                  /*artifact_store=*/nullptr, /*layers=*/nullptr,
                                  /*roles_table=*/nullptr, /*compact_partition_count=*/0,
                                  /*deferred_tool_summary=*/nullptr, &calibrated, facts);
        text = capture.text();
    }
    CHECK(text.find("utf8_bytes_div4") != std::string::npos);
    CHECK(text.find("1.59") == std::string::npos);  // v2 校准系数不乘不显
    CHECK(text.find("结果仓") != std::string::npos);
    CHECK(text.find("3 枚") != std::string::npos);
    CHECK(text.find("结构压缩层") != std::string::npos);
}

TEST_CASE("v3 会话:最近请求预算分栏,三枚值各有其名") {
    app::ContextSessionFacts facts = V3Facts();
    runtime::PreRequestBudget budget;
    budget.context_window_tokens = 1048576;
    budget.declared_max_output_tokens = 524288;   // provider 声明
    budget.policy_reserve_tokens = 32768;         // 策略预留(封顶后)
    budget.final_reserve_tokens = 32768;          // 本次判定预留
    budget.effective_output_limit_tokens = 511635;  // 降级后实发
    budget.protocol_headroom_tokens = 512;
    facts.last_request_budget = &budget;

    cli::ContextTracker tracker(1048576);
    const cli::Theme theme;
    std::string text;
    {
        OutputCapture capture;
        app::HandleContextCommand("", tracker, 100, 200, 300, theme, /*cache_epoch=*/2,
                                  /*main_profile=*/nullptr, /*usage_ledger=*/nullptr,
                                  /*artifact_store=*/nullptr, /*layers=*/nullptr,
                                  /*roles_table=*/nullptr, /*compact_partition_count=*/0,
                                  /*deferred_tool_summary=*/nullptr,
                                  /*token_calibration=*/nullptr, facts);
        text = capture.text();
    }
    // 524288 / 32768 / 511635 三枚值同屏可解释(单内验收),不再混作一个
    // "预留"。
    CHECK(text.find("最近请求预算") != std::string::npos);
    CHECK(text.find("524288") != std::string::npos);
    CHECK(text.find("32768") != std::string::npos);
    CHECK(text.find("511635") != std::string::npos);
}

TEST_CASE("v3 会话:没发过请求就明说,不冒充") {
    app::ContextSessionFacts facts = V3Facts();
    facts.last_request_budget = nullptr;
    cli::ContextTracker tracker(100000);
    const cli::Theme theme;
    std::string text;
    {
        OutputCapture capture;
        app::HandleContextCommand("", tracker, 100, 200, 300, theme, /*cache_epoch=*/1,
                                  /*main_profile=*/nullptr, /*usage_ledger=*/nullptr,
                                  /*artifact_store=*/nullptr, /*layers=*/nullptr,
                                  /*roles_table=*/nullptr, /*compact_partition_count=*/0,
                                  /*deferred_tool_summary=*/nullptr,
                                  /*token_calibration=*/nullptr, facts);
        text = capture.text();
    }
    CHECK(text.find("尚未发请求") != std::string::npos);
}

TEST_CASE("v2 会话:旧口径一行不改——校准行与 artifact 层照旧") {
    app::ContextSessionFacts facts;  // v2:全默认
    cli::ContextTracker tracker(100000);
    const cli::Theme theme;
    agent::TokenCalibrationStatus calibrated;
    calibrated.calibrated = false;

    std::string text;
    {
        OutputCapture capture;
        app::HandleContextCommand("", tracker, 100, 200, 300, theme, /*cache_epoch=*/1,
                                  /*main_profile=*/nullptr, /*usage_ledger=*/nullptr,
                                  /*artifact_store=*/nullptr, /*layers=*/nullptr,
                                  /*roles_table=*/nullptr, /*compact_partition_count=*/0,
                                  /*deferred_tool_summary=*/nullptr, &calibrated, facts);
        text = capture.text();
    }
    CHECK(text.find("未校准") != std::string::npos);      // v2 校准行照旧
    CHECK(text.find("utf8_bytes_div4") == std::string::npos);  // v3 口径行不出现
    CHECK(text.find("结构压缩层") == std::string::npos);
}
