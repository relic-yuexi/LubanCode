// 整轮逐步 usage 的分角色记账(骨架拆解反弹·问题 2)单测:折算与兜底
// 规则原先内联在 TerminalSessionController::RunSessionTurn 收尾,现在是一只
// 自由函数,喂几枚 step 就能断言台账——不必起会话、不必造假 backend。
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "agent/model_router.hpp"    // ModelUsageLedger/ModelRole
#include "app/turn_usage_account.hpp"
#include "runtime/turn_runtime.hpp"  // StepUsageRecord

namespace {

lubancode::runtime::StepUsageRecord Step(const std::string& model, std::int64_t input,
                                         std::int64_t cache_read, std::int64_t output,
                                         std::int64_t reasoning, bool reported) {
    lubancode::runtime::StepUsageRecord record;
    record.model = model;
    record.input_tokens = input;
    record.cache_read_tokens = cache_read;
    record.output_tokens = output;
    record.reasoning_tokens = reasoning;
    record.reported = reported;
    return record;
}

}  // namespace

TEST_CASE("逐步 usage 折进 normal 档:计数、总输入(含缓存)、兜底模型名") {
    lubancode::agent::ModelUsageLedger ledger;
    const std::vector<lubancode::runtime::StepUsageRecord> steps = {
        Step("model-a", 100, 40, 10, 5, true),
        Step("", 1, 0, 2, 0, true),   // 模型没报:兜底当前模型名
        Step("model-b", 7, 3, 4, 0, false),  // 未回报:token 三项按 0 收账
    };
    lubancode::app::RecordTurnUsageSteps(ledger, steps, "current-model");

    const auto& roles = ledger.by_role();
    REQUIRE(roles.find(lubancode::agent::ModelRole::Normal) != roles.end());
    const lubancode::agent::ModelUsageEntry& entry = roles.at(lubancode::agent::ModelRole::Normal);
    CHECK(entry.calls == 3);
    // 已回报的两笔:完整输入 = input + cache_read + cache_creation。
    // (100+40) + (1+0) = 141;未回报那笔不进 token 账。
    CHECK(entry.input_tokens == 141);
    CHECK(entry.output_tokens == 12);
    // 未计时报 0(duration_ms 传 0,照记不猜)。
    CHECK(entry.duration_ms == 0);
    CHECK(entry.reported);
    // last_model 记最后一笔的模型;最后一笔未回报但模型名照写。
    CHECK(entry.last_model == "model-b");
    // 兜底名真被用过:第二笔的模型空,落到 current-model。
    lubancode::agent::ModelUsageLedger single;
    lubancode::app::RecordTurnUsageSteps(single, {Step("", 5, 0, 1, 0, true)}, "current-model");
    CHECK(single.by_role().at(lubancode::agent::ModelRole::Normal).last_model == "current-model");
}

TEST_CASE("空轮零笔:台账不动") {
    lubancode::agent::ModelUsageLedger ledger;
    lubancode::app::RecordTurnUsageSteps(ledger, {}, "current-model");
    CHECK(ledger.by_role().empty());
}
