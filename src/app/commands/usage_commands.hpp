// /usage(Token 账本单 A2):把 Journal 里的 Token 账本投影成用户可读的
// 报告。只透明展示,不加限额、不劝省——消耗列得明明白白就是本命。
//
// 形态(§10.1,本批落 session 面):
//   /usage                      当前会话(active,未封口标 provisional)
//   /usage session <session-id> 指定会话
//   /usage --by model|purpose|run|outcome   换一张分账表
//   /usage --json               JSON 输出(报告面 schema,派生可删可重算)
// day/week/workspace/all 的跨场汇总属后续批次,敲了明说,不冒充。
//
// 口径戒律(A1 落的,本件照守):
//   - provider 没报 usage 写 unknown,不写 0;
//   - compact/title 一类旁路请求算进总账(purpose 分账列明);
//   - Journal 侧逐请求账不可得时,报告明说,降级给内存粗账
//     (ModelUsageLedger,按角色累计)并明示口径差异——内存账没有
//     purpose 分账、没有逐请求 coverage,不能悄悄换口径冒充实测。
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "accounting/pricing_table.hpp"
#include "accounting/session_usage_reader.hpp"
#include "accounting/usage_aggregate.hpp"
#include "app/commands/command_flow.hpp"
#include "cli/slash_commands.hpp"
#include "cli/theme.hpp"

namespace lubancode::agent {
class ModelUsageLedger;
}

namespace lubancode::runtime {
class TrajectorySessionLedger;
}

namespace lubancode::app {

// ---------------- 纯函数(单测钉住) ----------------

// /usage 的二级参数拆词(纯解析,不管 session 存不存在)。
struct ParsedUsageCommand {
    enum class Scope { ActiveSession, NamedSession };
    enum class By { None, Model, Purpose, Run, Outcome };
    Scope scope = Scope::ActiveSession;
    std::string session_id;  // NamedSession 的目标
    By by = By::None;
    bool json = false;
    bool invalid = false;    // 认不得的词;bad_word 给提示
    std::string bad_word;
    // day/week/workspace/all:本批未落的口径,原词记下(handler 提示后续
    // 批次,不当错也不冒充)。
    std::string later_scope;
};
ParsedUsageCommand ParseUsageCommand(const std::string& args);

// 整数 micros -> "0.284" 形态的金额串(十进制定长拼装,全程整数,禁 float)。
// currency 为空时只给数字串;micros=0 且 priced=false 由调用方写 not_priced。
std::string FormatMicrosAmount(std::int64_t micros, const std::string& currency);

// 给一批 sample 记账价(§6.3):table 为空整体 not_priced。request_day 取
// session_id 前 8 位(YYYYMMDD,session id 的头一段;不足 8 位按空算,
// 价格表的生效日比较自会判不命中)。
void ApplyCostEstimates(std::vector<lubancode::accounting::UsageSample>& samples,
                        const std::optional<lubancode::accounting::PricingTable>& table);

// 报告模型(渲染的唯一入参;handler 装好,渲染零 IO)。
struct UsageReportModel {
    std::string session_id;
    std::string workspace_key;
    std::string status;        // session.json status;读不到 unknown
    bool provisional = false;  // active/未封口:数字如实,成色注明
    // 价格表口径(§6.3 四条线):没配给 nullopt,note 说明。
    std::optional<lubancode::accounting::PricingTable> pricing;
    std::string pricing_note;  // "未配价格表" / 表 id / 坏表说明
    lubancode::accounting::UsageAggregate aggregate;
    ParsedUsageCommand::By by = ParsedUsageCommand::By::None;
};

// 终端人话(§7.4 默认画面;--by 时换分账表)。行不带换行符。
std::vector<std::string> FormatUsageReport(const UsageReportModel& model);

// JSON 输出(schema: lubancode.usage.report v1;报告面合同,派生可删可重算)。
nlohmann::json BuildUsageReportJson(const UsageReportModel& model);

// ---------------- 执行(IO) ----------------

// 价格表装载:<home_lubancode>/pricing.json。没配给 nullopt + "未配价格表";
// 坏表给 nullopt + 坏处说明(不炸命令,token 照报,费用 not_priced)。
struct LoadedPricing {
    std::optional<lubancode::accounting::PricingTable> table;
    std::string note;
};
LoadedPricing LoadPricingTable(const std::optional<std::string>& home_lubancode);

// 会话侧材料包(handler 从 SlashDispatchContext 装好)。
struct UsageCommandContext {
    const lubancode::cli::Theme& theme;
    // flag 开的会话递账本(active session 的 session_dir 从这取);
    // nullptr = 轨迹没开,走内存粗账降级。
    lubancode::runtime::TrajectorySessionLedger* trajectory = nullptr;
    // 指定 session 的查找根(= active session_dir 的 sessions/ 兄弟层)。
    std::filesystem::path sessions_root;
    // 内存粗账(flag 关的降级面;可空 = 连 ModelRouter 都没接)。
    const lubancode::agent::ModelUsageLedger* memory_ledger = nullptr;
    std::optional<std::string> home_lubancode;
};

// /usage 主入口(IO 侧):解析 -> 装报告模型 -> 渲染/JSON。
void HandleUsageCommand(const std::string& args, const UsageCommandContext& context);

// 命令分派注册制:/usage 的分派位。
struct SlashDispatchContext;
CommandFlow HandleSlashUsage(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);

}  // namespace lubancode::app
