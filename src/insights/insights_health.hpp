// /doctor insights(Token 账本单 §10.4/A5):insights 管线的健康检查。
//
// 查:轨迹开没开、reader 版本口径、派生目录权限与磁盘余量、最近一次
// 成功报告、stale/corrupt 摘要数、价格表口径、HTML renderer 自检、
// model review 默认关。不调模型,也不重建报告。
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "insights/insights_generate.hpp"

namespace lubancode::insights {

struct InsightsHealthLine {
    std::string name;
    char status = ' ';  // ' ' ok / '!' warn / 'x' fail
    std::string detail;
};

struct InsightsHealthInput {
    bool trajectory_on = false;
    std::optional<std::filesystem::path> insights_home;  // ~/.lubancode/insights
    std::vector<InsightsWorkspaceRef> workspaces;        // 检查范围(当前 ws 或全部)
    std::string now_yyyymmdd;  // 摘要扫描的日期窗(空 = 不按日期过滤)
    int since_days = 30;
    // 价格表口径由 app 层配(LoadPricingTable 在 app,领域层不吃 app)。
    bool pricing_loaded = false;
    std::string pricing_note;
};

std::vector<InsightsHealthLine> CheckInsightsHealth(const InsightsHealthInput& input);

}  // namespace lubancode::insights
