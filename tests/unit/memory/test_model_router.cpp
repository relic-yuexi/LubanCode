// 模型分工第一期的单测:三角色解析与回退链、任务映射、compact_model
// 兼容别名、usage 分角色记账、/model roles 短表与配置解析(规格"测试"
// 节:三角色解析、配置优先级和 cheap/lao -> normal 回退、compact_model
// 旧配置的兼容与冲突提示、usage 分角色记账)。
#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "agent/model_router.hpp"
#include "app/model_router.hpp"
#include "app/session_title.hpp"
#include "cli/line_editor.hpp"
#include "config/config.hpp"
#include "platform/text_encoding.hpp"

namespace {

using lubancode::agent::ModelRole;
using lubancode::agent::ModelRoleSpec;
using lubancode::agent::ModelRouteTable;
using lubancode::agent::ResolveModelRoutes;
using lubancode::agent::TaskKind;

ModelRoleSpec Spec(const std::string& model, const std::string& source = "项目配置",
                   const std::string& provider = std::string(), const std::string& effort = std::string()) {
    ModelRoleSpec spec;
    spec.model = model;
    spec.source = source;
    spec.provider = provider;
    spec.effort = effort;
    return spec;
}

// 空快捷构造:未配置。
ModelRoleSpec EmptySpec() {
    return {};
}

lubancode::config::ConfigResult MergeFromJson(const std::string& json_text) {
    const auto parsed = lubancode::config::ParseFileConfigJson(json_text, "test.json");
    REQUIRE(parsed.has_value());
    const auto merged = lubancode::config::MergeConfig(
        lubancode::config::LubancodeEnvValues{}, std::optional<lubancode::config::FileConfig>{*parsed});
    REQUIRE(merged.has_value());
    return *merged;
}

}  // namespace

TEST_CASE("回退链:normal 未配置用会话模型,cheap/lao 回落 normal") {
    SUBCASE("全未配置") {
        const auto table = ResolveModelRoutes(EmptySpec(), EmptySpec(), EmptySpec(), "qwen-session", "");
        CHECK(table.normal.model == "qwen-session");
        CHECK(table.normal.source == "当前会话");
        CHECK(table.cheap.model == "qwen-session");
        CHECK(table.cheap.fell_back_to_normal);
        CHECK(table.lao.model == "qwen-session");
        CHECK(table.lao.fell_back_to_normal);
        // 回落的来源必须写明"回落到 normal",不重印同名让用户猜。
        CHECK(table.cheap.source.find("回落到 normal") != std::string::npos);
    }
    SUBCASE("只配 normal") {
        const auto table = ResolveModelRoutes(Spec("m-normal"), EmptySpec(), EmptySpec(), "qwen-session", "");
        CHECK(table.normal.model == "m-normal");
        CHECK(!table.normal.fell_back_to_normal);
        CHECK(table.cheap.model == "m-normal");
        CHECK(table.cheap.fell_back_to_normal);
    }
    SUBCASE("三档全配") {
        const auto table = ResolveModelRoutes(Spec("m-normal"), Spec("m-cheap"), Spec("m-lao"), "qwen-session",
                                              "prov");
        CHECK(table.normal.model == "m-normal");
        CHECK(table.cheap.model == "m-cheap");
        CHECK(!table.cheap.fell_back_to_normal);
        CHECK(table.lao.model == "m-lao");
        // provider 留空继承活跃端。
        CHECK(table.cheap.provider == "prov");
    }
    SUBCASE("provider 显式跨端") {
        const auto table = ResolveModelRoutes(Spec("m-normal"), Spec("m-cheap", "项目配置", "local_fast", "low"),
                                              EmptySpec(), "qwen-session", "local");
        CHECK(table.cheap.provider == "local_fast");
        CHECK(table.cheap.effort == "low");
    }
}

TEST_CASE("任务映射:角色跟 TaskKind 走,不跟 agent 身份") {
    using lubancode::agent::DefaultRoleForTask;
    CHECK(DefaultRoleForTask(TaskKind::NormalTurn) == ModelRole::Normal);
    CHECK(DefaultRoleForTask(TaskKind::Plan) == ModelRole::Lao);
    CHECK(DefaultRoleForTask(TaskKind::Compact) == ModelRole::Cheap);
    CHECK(DefaultRoleForTask(TaskKind::CompactRepair) == ModelRole::Normal);
    CHECK(DefaultRoleForTask(TaskKind::MemoryExtract) == ModelRole::Cheap);
    CHECK(DefaultRoleForTask(TaskKind::RetrievalExpansion) == ModelRole::Cheap);
    CHECK(DefaultRoleForTask(TaskKind::Classification) == ModelRole::Cheap);
    CHECK(DefaultRoleForTask(TaskKind::SessionTitle) == ModelRole::Cheap);
    CHECK(DefaultRoleForTask(TaskKind::ResumeSummary) == ModelRole::Cheap);
}

TEST_CASE("RouteFor:按任务取路由") {
    const auto table = ResolveModelRoutes(Spec("m-normal"), Spec("m-cheap"), Spec("m-lao"), "qwen-session", "p");
    CHECK(table.RouteFor(TaskKind::NormalTurn).model == "m-normal");
    CHECK(table.RouteFor(TaskKind::Plan).model == "m-lao");
    CHECK(table.RouteFor(TaskKind::Compact).model == "m-cheap");
    CHECK(table.RouteFor(TaskKind::CompactRepair).model == "m-normal");
}

TEST_CASE("配置解析:三字段与 model_roles 段,空串/null 归一为未配置") {
    SUBCASE("shorthand 三字段") {
        const auto result = MergeFromJson(R"({
            "normal_model": "qwen3.8-27b",
            "cheap_model": "qwen3.8-4b",
            "lao_model": "qwen3.8-72b"
        })");
        CHECK(result.config.normal_model == "qwen3.8-27b");
        CHECK(result.config.cheap_model == "qwen3.8-4b");
        CHECK(result.config.lao_model == "qwen3.8-72b");
    }
    SUBCASE("空串与显式 null 都算未配置") {
        const auto result = MergeFromJson(R"({
            "normal_model": "",
            "cheap_model": null,
            "lao_model": "qwen3.8-72b"
        })");
        CHECK(result.config.normal_model.empty());
        CHECK(result.config.cheap_model.empty());
        CHECK(result.config.lao_model == "qwen3.8-72b");
    }
    SUBCASE("model_roles 高级段:窗口与输出上限换算") {
        const auto result = MergeFromJson(R"({
            "model_roles": {
                "normal": {"provider": "local", "model": "qwen3.8-27b", "effort": "high", "context_window": "512k", "max_output_tokens": "16k"},
                "cheap": {"provider": "local_fast", "model": "qwen3.8-4b", "effort": "low"},
                "lao": {"provider": "remote", "model": "reasoner-large", "effort": "xhigh"}
            }
        })");
        CHECK(result.config.model_roles.normal.model == "qwen3.8-27b");
        CHECK(*result.config.model_roles.normal.context_window == 512000);
        CHECK(*result.config.model_roles.normal.max_output_tokens == 16000);
        CHECK(result.config.model_roles.cheap.provider == "local_fast");
        CHECK(result.model_role_notices.empty());
    }
    SUBCASE("认不得的角色键报错") {
        const auto parsed = lubancode::config::ParseFileConfigJson(R"({"model_roles": {"fast": {}}})",
                                                                   "test.json");
        CHECK(!parsed.has_value());
    }
    SUBCASE("shorthand 与高级段撞车记提示,高级段优先") {
        const auto result = MergeFromJson(R"({
            "cheap_model": "old-cheap",
            "model_roles": {"cheap": {"model": "new-cheap"}}
        })");
        REQUIRE(result.config.model_roles.cheap.model == "new-cheap");
        const auto specs = lubancode::app::BuildRoleSpecs(result);
        CHECK(specs[1].model == "new-cheap");
        bool noticed = false;
        for (const auto& notice : result.model_role_notices) {
            if (notice.find("model_roles") != std::string::npos) {
                noticed = true;
            }
        }
        CHECK(noticed);
    }
}

TEST_CASE("compact_model 兼容别名:只顶压缩,不接管记忆与标题") {
    SUBCASE("只写 compact_model(cheap 未配)") {
        const auto result = MergeFromJson(R"({"compact_model": "old-compactor"})");
        const auto specs = lubancode::app::BuildRoleSpecs(result);
        const auto table = ResolveModelRoutes(specs[0], specs[1], specs[2], "session-model", "p");
        CHECK(!table.compact_legacy_override.has_value());  // 表本身不带,服务层再折
        // 服务层的折法:cheap 回落 normal 且 compact_model 非空 → 顶替压缩。
        // 这里直接用 Table 逻辑等价的手工路径验证 RouteFor 语义:
        // compact_legacy_override 在场时 Compact/CompactRepair 用它,其余任务不受影响。
        ModelRouteTable with_legacy = table;
        lubancode::agent::ModelRoute legacy = table.normal;
        legacy.model = "old-compactor";
        with_legacy.compact_legacy_override = legacy;
        CHECK(with_legacy.RouteFor(TaskKind::Compact).model == "old-compactor");
        CHECK(with_legacy.RouteFor(TaskKind::CompactRepair).model == "old-compactor");
        // 关键:记忆抽取、标题、resume 摘要照走 cheap 的有效值(= normal),
        // 旧字段不许突然接管(规格"旧字段只影响 compact")。
        CHECK(with_legacy.RouteFor(TaskKind::MemoryExtract).model == "session-model");
        CHECK(with_legacy.RouteFor(TaskKind::SessionTitle).model == "session-model");
        CHECK(with_legacy.RouteFor(TaskKind::ResumeSummary).model == "session-model");
        CHECK(with_legacy.RouteFor(TaskKind::NormalTurn).model == "session-model");
    }
    SUBCASE("compact_model 与 cheap_model 同写:报冲突") {
        const auto result = MergeFromJson(R"({"compact_model": "a", "cheap_model": "b"})");
        bool conflict = false;
        for (const auto& notice : result.model_role_notices) {
            if (notice.find("compact_model") != std::string::npos &&
                notice.find("cheap_model") != std::string::npos) {
                conflict = true;
            }
        }
        CHECK(conflict);
    }
}

TEST_CASE("BuildRoleSpecs:高级段优先于 shorthand,来源句分得清") {
    const auto result = MergeFromJson(R"({
        "normal_model": "n1",
        "lao_model": "l1",
        "model_roles": {"lao": {"model": "l2", "provider": "remote", "effort": "xhigh"}}
    })");
    const auto specs = lubancode::app::BuildRoleSpecs(result);
    REQUIRE(specs.size() == 3);
    CHECK(specs[0].model == "n1");                       // normal 走 shorthand
    CHECK(specs[1].model.empty());                       // cheap 两级都没配
    CHECK(specs[2].model == "l2");                       // lao 高级段压过 shorthand
    CHECK(specs[2].provider == "remote");
    CHECK(specs[2].effort == "xhigh");
    CHECK(!specs[0].source.empty());  // shorthand 带来源句(具体文案走 i18n,不钉字串)
    CHECK(specs[2].source.find("model_roles") != std::string::npos);
}

TEST_CASE("usage 分角色记账:Record/ReportLines/回退留痕") {
    lubancode::agent::ModelUsageLedger ledger;
    lubancode::api::Usage usage;
    usage.input_tokens = 1000;
    usage.output_tokens = 200;
    ledger.Record(ModelRole::Cheap, "m-cheap", usage, 1500, true);
    ledger.Record(ModelRole::Cheap, "m-cheap", usage, 500, true);
    ledger.Record(ModelRole::Normal, "m-normal", usage, 0, false);

    const auto lines = ledger.ReportLines();
    // 三角色固定列全(问题 6):零调用的 lao 也露脸,不隐藏。
    REQUIRE(lines.size() == 3);
    // 顺序固定 cheap -> normal -> lao,好对照 /model roles。
    CHECK(lines[0].find("cheap") != std::string::npos);
    CHECK(lines[0].find("m-cheap") != std::string::npos);
    CHECK(lines[0].find("2 次调用") != std::string::npos);
    CHECK(lines[0].find("2000") != std::string::npos);  // 输入累计
    CHECK(lines[1].find("usage 未报告") != std::string::npos);
    CHECK(lines[2].find("lao") != std::string::npos);
    CHECK(lines[2].find("0 次调用") != std::string::npos);
    CHECK(lines[2].find("本场未触发") != std::string::npos);
    CHECK(lines[2].find("Plan/规划任务") != std::string::npos);

    ledger.RecordFallback(TaskKind::Compact, ModelRole::Cheap, ModelRole::Normal, "provider 超时");
    REQUIRE(ledger.fallback_notes().size() == 1);
    CHECK(ledger.fallback_notes()[0].find("cheap") != std::string::npos);
    CHECK(ledger.fallback_notes()[0].find("normal") != std::string::npos);
    // from == to 不是回退,不记。
    ledger.RecordFallback(TaskKind::Compact, ModelRole::Normal, ModelRole::Normal, "x");
    CHECK(ledger.fallback_notes().size() == 1);
}

TEST_CASE("分角色账:新会话三角色全见 0 次,职责各写一句(问题 6)") {
    lubancode::agent::ModelUsageLedger ledger;
    const auto table =
        ResolveModelRoutes(Spec("m-normal"), Spec("m-cheap"), Spec("m-lao"), "qwen-session", "prov");
    const auto lines = ledger.ReportLines(&table);
    REQUIRE(lines.size() == 3);
    for (const auto& line : lines) {
        CHECK(line.find("0 次调用") != std::string::npos);
        CHECK(line.find("本场未触发") != std::string::npos);
    }
    CHECK(lines[0].find("m-cheap") != std::string::npos);   // 零调用也摆有效路由的模型
    CHECK(lines[0].find("后台采样") != std::string::npos);  // cheap 职责
    CHECK(lines[1].find("m-normal") != std::string::npos);
    CHECK(lines[1].find("普通对话与实现") != std::string::npos);
    CHECK(lines[2].find("m-lao") != std::string::npos);
}

TEST_CASE("分角色账:只调 normal 时 cheap/lao 仍列 0 次;回落行内写明(问题 6)") {
    lubancode::agent::ModelUsageLedger ledger;
    lubancode::api::Usage usage;
    usage.input_tokens = 100;
    ledger.Record(ModelRole::Normal, "m-normal", usage, 1200, true);
    // cheap/lao 未配置 -> 回落 normal(与 /model roles 同一张表、同一份 source)。
    const auto table = ResolveModelRoutes(Spec("m-normal"), EmptySpec(), EmptySpec(), "qwen-session", "prov");
    const auto lines = ledger.ReportLines(&table);
    REQUIRE(lines.size() == 3);
    CHECK(lines[0].find("cheap") != std::string::npos);
    CHECK(lines[0].find("0 次调用") != std::string::npos);
    CHECK(lines[0].find("回落到 normal") != std::string::npos);  // 回落写明,不重印同名装独立配置
    CHECK(lines[1].find("1 次调用") != std::string::npos);
    CHECK(lines[2].find("lao") != std::string::npos);
    CHECK(lines[2].find("0 次调用") != std::string::npos);
    CHECK(lines[2].find("回落到 normal") != std::string::npos);
    // 配了独立 lao 就不写回落。
    const auto table2 = ResolveModelRoutes(Spec("m-normal"), EmptySpec(), Spec("m-lao"), "qwen-session", "prov");
    const auto lines2 = ledger.ReportLines(&table2);
    CHECK(lines2[2].find("m-lao") != std::string::npos);
    CHECK(lines2[2].find("回落到 normal") == std::string::npos);
}

TEST_CASE("/model roles 短表:回落行写明'回落到 normal'") {
    const auto table = ResolveModelRoutes(Spec("m-normal"), EmptySpec(), Spec("m-lao"), "qwen-session", "prov");
    const auto lines = lubancode::app::FormatModelRolesTable(table);
    REQUIRE(lines.size() == 4);  // 表头 + 三行
    CHECK(lines[0].find("provider") != std::string::npos);
    CHECK(lines[0].find("来源") != std::string::npos);
    // cheap 回落:来源列必须点名回落,不许重印 normal 的名字装没事。
    CHECK(lines[1].find("回落到 normal") != std::string::npos);
    CHECK(lines[2].find("m-normal") != std::string::npos);   // normal 自己
    CHECK(lines[3].find("m-lao") != std::string::npos);

    // 中文表头按终端列宽占两格。各行 provider 列须从同一显示列起步，
    // 不能拿 UTF-8 码点数冒充终端宽度。
    const auto provider_column = [](const std::string& line, const std::string& value) {
        const std::size_t offset = line.find(value);
        REQUIRE(offset != std::string::npos);
        return lubancode::cli::DisplayWidthUtf8(line.substr(0, offset));
    };
    const std::size_t expected_provider_column = provider_column(lines[0], "provider");
    CHECK(provider_column(lines[1], "prov") == expected_provider_column);
    CHECK(provider_column(lines[2], "prov") == expected_provider_column);
    CHECK(provider_column(lines[3], "prov") == expected_provider_column);
}

TEST_CASE("标题清洗:剥围栏/引号、压空白、按码点限长") {
    using lubancode::app::SanitizeTitle;
    CHECK(SanitizeTitle("修登录超时") == "修登录超时");
    CHECK(SanitizeTitle("```\n修登录超时\n```") == "修登录超时");
    CHECK(SanitizeTitle("\"调研 向量库\"") == "调研 向量库");
    CHECK(SanitizeTitle("多行\n标题  挤压") == "多行 标题 挤压");
    CHECK(SanitizeTitle("") == "");
    // 中文按码点截断:5 个字限 3,绝不在多字节中腰劈开。
    const std::string truncated = SanitizeTitle("一二三四五", 3);
    CHECK(truncated == "一二三");
}

TEST_CASE("标题精炼:只喂首问,600 字节刀口不留下半个汉字") {
    struct CaptureBackend final : lubancode::api::Backend {
        lubancode::api::Request captured;

        std::expected<void, lubancode::api::Error> send_stream(
            const lubancode::api::Request& request,
            const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
            const std::atomic<bool>*) override {
            captured = request;
            on_event(lubancode::api::TextDelta{"边界安全"});
            on_event(lubancode::api::ContentBlockDone{0});
            on_event(lubancode::api::MessageDone{"end_turn", {}});
            return {};
        }
    } backend;

    std::string text(599, 'a');
    text += "中";  // 第 600 字节正落在这枚三字节字符的腰上。

    const auto title = lubancode::app::RefineSessionTitle(backend, "test-model", "", text,
                                                          /*timeout_secs=*/2);
    REQUIRE(title.has_value());
    CHECK(*title == "边界安全");
    REQUIRE(backend.captured.messages.size() == 1);
    REQUIRE(backend.captured.messages[0].content.size() == 1);
    const auto& sent = std::get<lubancode::api::TextBlock>(backend.captured.messages[0].content[0]).text;
    CHECK(lubancode::platform::IsValidUtf8(sent));
    // 原请求仍只取 UTF-8 安全的首问截段;前面附一枚本地主题线索,不带
    // 助手回复或工具转写的尾巴。
    CHECK(sent == "本地主题线索: " + std::string(48, 'a') +
                      "\n原请求: " + std::string(599, 'a'));
    // 单子预算:输出上限收紧到 24,reasoning 没配档位时压到最低档 low。
    REQUIRE(backend.captured.max_tokens.has_value());
    CHECK(*backend.captured.max_tokens == lubancode::app::kTitleRefineMaxTokens);
    CHECK(backend.captured.max_tokens.value_or(0) <= 24);
    CHECK(backend.captured.reasoning_effort == "low");
    // 路由带档位就按配的来,不覆盖用户的显式配置。
    CaptureBackend backend2;
    lubancode::app::RefineSessionTitle(backend2, "test-model", "minimal", text, /*timeout_secs=*/2);
    CHECK(backend2.captured.reasoning_effort == "minimal");
}

TEST_CASE("本地临时标题:取首行、清空白、按码点限长") {
    using lubancode::app::LocalSessionTitle;
    CHECK(LocalSessionTitle("做一个图书管理系统,node 前端") == "做一个图书管理系统,node 前端");
    // 多行粘贴只取首行。
    CHECK(LocalSessionTitle("第一行是题眼\n第二行是细节\n") == "第一行是题眼");
    // 连续空白压成单空格,首尾空白剥掉。
    CHECK(LocalSessionTitle("  调研   向量库  选型 \t") == "调研 向量库 选型");
    // 中文按码点截断:24 字限 10,绝不在多字节中腰劈开。
    CHECK(LocalSessionTitle("一二三四五六七八九十甲乙丙丁", 10) == "一二三四五六七八九十");
    // 首问没剩可看的字:空串(调用方当"没起出来"处理)。
    CHECK(LocalSessionTitle("   \n第二行") == "");
    CHECK(LocalSessionTitle("") == "");
}

TEST_CASE("ModelRouterService:同 provider 走主 backend,跨 provider 建裸 client") {
    using lubancode::app::ModelRouterService;
    // 假 backend:主链的替身。
    struct FakeBackend final : public lubancode::api::Backend {
        int hits = 0;
        std::expected<void, lubancode::api::Error> send_stream(
            const lubancode::api::Request&,
            const std::function<void(const lubancode::api::StreamEvent&)>&,
            const std::atomic<bool>*) override {
            ++hits;
            return {};
        }
    };
    FakeBackend main_backend;
    auto current_model = std::make_shared<std::string>("session-model");
    std::string active_provider = "local";

    auto result = MergeFromJson(R"({
        "providers": [
            {"name": "local", "base_url": "http://localhost:1", "wire": "anthropic", "model": "n1"},
            {"name": "local_fast", "base_url": "http://localhost:2", "wire": "chat_completions", "model": "c1"}
        ],
        "active_provider": "local",
        "cheap_model": "c1",
        "model_roles": {"cheap": {"provider": "local_fast", "model": "fast-m", "effort": "low"}}
    })");

    ModelRouterService service(result, main_backend, current_model, active_provider);
    SUBCASE("compact 走跨 provider 的 fast 路由") {
        const auto routed = service.Route(TaskKind::Compact);
        REQUIRE(routed.backend != nullptr);
        CHECK(routed.route.model == "fast-m");
        CHECK(routed.route.provider == "local_fast");
        CHECK(routed.route.effort == "low");
        CHECK(routed.backend != &main_backend);  // 另建了裸 client
    }
    SUBCASE("普通 turn 复用主 backend") {
        const auto routed = service.Route(TaskKind::NormalTurn);
        CHECK(routed.route.model == "session-model");
        CHECK(routed.backend == &main_backend);
    }
    SUBCASE("后台路由另造独占 backend,当前端也不借主 client") {
        auto routed = service.RouteDetached(TaskKind::SessionTitle);
        REQUIRE(routed.backend != nullptr);
        CHECK(routed.route.model == "fast-m");
        CHECK(routed.route.provider == "local_fast");

        auto normal = service.RouteDetached(TaskKind::NormalTurn);
        REQUIRE(normal.backend != nullptr);
        CHECK(normal.route.model == "session-model");
        CHECK(normal.backend.get() != &main_backend);
    }
    SUBCASE("provider 名找不到条目:backend 交空,不静默换名") {
        auto broken = MergeFromJson(R"({
            "providers": [{"name": "local", "base_url": "http://localhost:1", "wire": "anthropic", "model": "n1"}],
            "active_provider": "local",
            "model_roles": {"cheap": {"provider": "ghost", "model": "m"}}
        })");
        ModelRouterService ghost_service(broken, main_backend, current_model, active_provider);
        const auto routed = ghost_service.Route(TaskKind::Compact);
        CHECK(routed.route.model == "m");
        CHECK(routed.backend == nullptr);
        auto detached = ghost_service.RouteDetached(TaskKind::SessionTitle);
        CHECK(detached.backend == nullptr);
    }
}

// ---------------------------------------------------------------------------
// 标题触发提前单·哑门触发条件钉:标题精炼起飞前的两道静默门(路由模型空 /
// provider 找不到),此前拦下即无声无息——用户只见标题永远停在本地档。
// 这里钉的是"什么时候拦":门拦下后亮报与否是 controller 的活,判定本体
// 就是这两只查询。
// ---------------------------------------------------------------------------
TEST_CASE("标题精炼哑门: 会话模型空时 SessionTitle 路由 model 空(门二触发条件)") {
    using lubancode::app::ModelRouterService;
    struct NullBackend final : public lubancode::api::Backend {
        std::expected<void, lubancode::api::Error> send_stream(
            const lubancode::api::Request&,
            const std::function<void(const lubancode::api::StreamEvent&)>&,
            const std::atomic<bool>*) override {
            return {};
        }
    };
    NullBackend backend;
    // 会话模型空(欢迎页空配置进主界面的形状)+ cheap/normal 全未配:
    // SessionTitle 回落 normal,normal 沿会话模型——一路空到底。
    auto current_model = std::make_shared<std::string>("");
    std::string active_provider = "local";
    const auto result = MergeFromJson(R"({
        "providers": [{"name": "local", "base_url": "http://localhost:1", "wire": "anthropic", "model": ""}],
        "active_provider": "local"
    })");
    ModelRouterService service(result, backend, current_model, active_provider);
    CHECK(service.RouteInfo(TaskKind::SessionTitle).model.empty());

    // 对照:会话模型补上(用户选了模型),同配置下路由立刻有 model——门
    // 开不开只看这一格。
    auto with_model = std::make_shared<std::string>("session-model");
    ModelRouterService live(result, backend, with_model, active_provider);
    CHECK_FALSE(live.RouteInfo(TaskKind::SessionTitle).model.empty());
}

// ---------------------------------------------------------------------------
// 记忆抽取 JSON 收口修复单 P1-A:SampleCall.output_schema 透传——设了 schema
// 的调用经 ModelRouterService::Sample 一站,复检账(schema_ok/schema_error)
// 真落回调用方手里;不设 schema 的旧行为一字不差。
// ---------------------------------------------------------------------------
TEST_CASE("ModelRouterService::Sample: SampleCall 透传 output_schema 到本地复检") {
    using lubancode::app::ModelRouterService;
    // 回坏 JSON 的假 backend:复检路只有 schema 真透传了才会翻 schema_ok。
    struct BadJsonBackend final : public lubancode::api::Backend {
        int calls = 0;
        std::expected<void, lubancode::api::Error> send_stream(
            const lubancode::api::Request&,
            const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
            const std::atomic<bool>*) override {
            ++calls;
            on_event(lubancode::api::TextDelta{"不是 JSON 的白话"});
            on_event(lubancode::api::ContentBlockDone{0});
            on_event(lubancode::api::MessageDone{"end_turn", lubancode::api::Usage{}});
            return {};
        }
    };
    BadJsonBackend backend;
    auto current_model = std::make_shared<std::string>("session-model");
    std::string active_provider = "local";
    const auto result = MergeFromJson(R"({
        "providers": [{"name": "local", "base_url": "http://localhost:1", "wire": "anthropic", "model": "n1"}],
        "active_provider": "local"
    })");
    ModelRouterService service(result, backend, current_model, active_provider);

    ModelRouterService::SampleCall call;
    call.system = "指令";
    lubancode::api::Message message;
    message.role = lubancode::api::Role::User;
    message.content.push_back(lubancode::api::TextBlock{"材料"});
    call.messages.push_back(std::move(message));
    call.max_tokens = 64;

    // 不设 schema:复检恒过(旧口径)。
    const auto plain = service.Sample(TaskKind::MemoryExtract, call);
    REQUIRE(plain.backend != nullptr);
    REQUIRE(plain.result.ok);
    CHECK(plain.result.schema_ok);
    CHECK(plain.recorded);

    // 设 schema:坏正文翻 schema_ok,错误文案带回——证明 schema 传到了
    // SampleModel 的本地复检。
    call.output_schema = nlohmann::json{{"type", "object"}};
    const auto checked = service.Sample(TaskKind::MemoryExtract, call);
    REQUIRE(checked.backend != nullptr);
    REQUIRE(checked.result.ok);  // 采样成了,复检只标记
    CHECK_FALSE(checked.result.schema_ok);
    CHECK(checked.result.schema_error.find("合法 JSON") != std::string::npos);
    CHECK(backend.calls == 2);  // 两发都真到了 backend
}

// ---------------------------------------------------------------------------
// HC-01(Provider 展开收口 + 角色后端缓存失效):两份合同。
//   一、展开合同:角色路由指向另一 provider 时,派生运行配置的连接与
//   能力字段逐项来自目标条目(走 config::ApplyProviderToRuntimeConfig
//   公共口,不再手抄清单);超时这类非连接全局项按原规则继承。
//   二、失效合同:跨 provider 的裸 client 缓存以连接指纹为版本键,目标
//   条目被编辑或删除后就地弃缓存——已删除的 Provider 不再交旧 client
//   发新请求。
// ---------------------------------------------------------------------------
TEST_CASE("HC-01 Provider 展开合同: 派生配置逐项来自目标端,非连接全局项继承") {
    // 活跃 A(anthropic)+ 目标 B(chat_completions,能力字段各立一档)。
    // 病灶口径:旧手抄展开漏了 stream_usage/think_param/native_web_search,
    // B 明明声明了,第一次 Route(B) 仍带 A 的值。
    auto result = MergeFromJson(R"({
        "providers": [
            {"name": "activeA", "base_url": "http://a.example", "wire": "anthropic", "model": "a-m"},
            {"name": "targetB", "base_url": "http://b.example", "wire": "chat_completions", "model": "b-m",
             "auth": "none", "stream_usage": true, "think_param": "effort_v2", "native_web_search": true,
             "reasoning_replay": "tool_episode", "reasoning_delta_field": "rd", "reasoning_replay_field": "rr",
             "extra_body": {"k": 1}, "extra_headers": {"X-B": "1"}, "context_window": 65536,
             "max_output_tokens": 4096, "supported_think_levels": ["low", "high"]}
        ],
        "active_provider": "activeA"
    })");
    // 活跃端的运行值与 B 的声明刻意相反:串用立刻露馅。
    result.config.stream_usage = false;
    result.config.think_param = "param_from_A";
    result.config.native_web_search = false;
    // 非连接全局项设怪值:展开口不碰,派生配置须原样继承。
    result.config.connect_timeout_ms = 12345;
    result.config.stream_idle_timeout_secs = 77;
    result.config.request_hard_timeout_secs = 321;

    const auto derived = lubancode::app::DeriveProviderRuntimeConfig(result.config, "targetB", "activeA");
    REQUIRE(derived.has_value());
    CHECK(derived->wire == lubancode::config::Wire::ChatCompletions);
    CHECK(derived->base_url == "http://b.example");
    CHECK(derived->model == "b-m");
    CHECK(derived->stream_usage == true);       // 来自 B,不再沿用 A 的 false
    CHECK(derived->think_param == "effort_v2"); // 来自 B
    CHECK(derived->native_web_search == true);  // 来自 B
    CHECK(derived->reasoning_replay == "tool_episode");
    CHECK(derived->reasoning_delta_field == "rd");
    CHECK(derived->reasoning_replay_field == "rr");
    CHECK(derived->extra_body == nlohmann::json{{"k", 1}});
    CHECK(derived->extra_headers.at("X-B") == "1");
    CHECK(derived->context_window_tokens == 65536);
    CHECK(derived->provider_max_output_tokens.has_value());
    CHECK(*derived->provider_max_output_tokens == 4096);
    CHECK(derived->provider_think_levels == std::vector<std::string>{"low", "high"});
    CHECK(derived->active_provider == "targetB");
    // 非连接全局项按原规则继承(公共展开口不写这些字段)。
    CHECK(derived->connect_timeout_ms == 12345);
    CHECK(derived->stream_idle_timeout_secs == 77);
    CHECK(derived->request_hard_timeout_secs == 321);

    SUBCASE("目标即活跃端: 原样副本,不施加条目") {
        // "原样" = 基础配置现状(测试的 MergeFromJson 不跑启动期的
        // ApplyConfiguredActiveProvider,顶层 base_url 仍为空),不是
        // 活跃条目 A 的值——重点是不施加任何条目、非连接全局项不动。
        const auto same = lubancode::app::DeriveProviderRuntimeConfig(result.config, "activeA", "activeA");
        REQUIRE(same.has_value());
        CHECK(same->base_url == result.config.base_url);
        CHECK(same->base_url != "http://b.example");  // 没拿 B 的条目
        CHECK(same->wire == result.config.wire);
        CHECK(same->stream_usage == false);  // 本测试改过的 A 侧运行值
        CHECK(same->think_param == "param_from_A");
        CHECK(same->connect_timeout_ms == 12345);
    }
    SUBCASE("目标名为空: 等同活跃端口径") {
        const auto empty = lubancode::app::DeriveProviderRuntimeConfig(result.config, "", "activeA");
        REQUIRE(empty.has_value());
        CHECK(empty->stream_usage == false);
    }
    SUBCASE("找不到条目: nullopt,调用方按'暂不可发'处理") {
        CHECK_FALSE(lubancode::app::DeriveProviderRuntimeConfig(result.config, "ghost", "activeA").has_value());
    }
}

TEST_CASE("HC-01 缓存失效合同: 编辑/删除目标端就地弃缓存,指纹外字段不误伤") {
    using lubancode::app::ModelRouterService;
    struct NullBackend final : public lubancode::api::Backend {
        std::expected<void, lubancode::api::Error> send_stream(
            const lubancode::api::Request&,
            const std::function<void(const lubancode::api::StreamEvent&)>&,
            const std::atomic<bool>*) override {
            return {};
        }
    };
    NullBackend main_backend;
    auto current_model = std::make_shared<std::string>("session-model");
    std::string active_provider = "activeA";

    // settings 命令(/provider set、/provider remove)落盘成功后改的是
    //同一份 config_result 内存——测试直接动 providers 向量,等价于"已
    // 提交的配置变更"。
    auto result = MergeFromJson(R"({
        "providers": [
            {"name": "activeA", "base_url": "http://a.example", "wire": "anthropic", "model": "a-m"},
            {"name": "targetB", "base_url": "http://b.example", "wire": "chat_completions", "model": "b-m"}
        ],
        "active_provider": "activeA",
        "model_roles": {"cheap": {"provider": "targetB", "model": "b-m"}}
    })");
    ModelRouterService service(result, main_backend, current_model, active_provider);

    const auto find_entry = [](lubancode::config::ConfigResult& config_result,
                               const std::string& name) -> lubancode::config::ProviderConfig* {
        for (auto& provider : config_result.config.providers) {
            if (provider.name == name) {
                return &provider;
            }
        }
        return nullptr;
    };

    // 未变:按名称复用同一只 client(Route/Sample 共这份缓存,此处只测
    // Route——Sample 会真发请求,不拿假域名练手)。
    lubancode::api::Backend* first = service.Route(TaskKind::Compact).backend;
    REQUIRE(first != nullptr);
    CHECK(service.Route(TaskKind::Compact).backend == first);

    // 编辑目标端(extra_body 变了 = /provider set 落盘成功后的形状)。
    // "重建发生了"不以指针不等为证——弃旧 shared_ptr 后新分配常复用
    // 同块内存(macOS 实测同址);这里钉三层:指纹函数本身认得这次编辑
    // (版本键变)、编辑后路由仍可用、新指纹下缓存重新稳定。
    lubancode::config::ProviderConfig* entry_b = find_entry(result, "targetB");
    REQUIRE(entry_b != nullptr);
    const std::string fingerprint_before = lubancode::config::ProviderConnectionFingerprint(*entry_b);
    entry_b->extra_body = nlohmann::json{{"changed", true}};
    CHECK(lubancode::config::ProviderConnectionFingerprint(*entry_b) != fingerprint_before);
    lubancode::api::Backend* rebuilt = service.Route(TaskKind::Compact).backend;
    REQUIRE(rebuilt != nullptr);
    CHECK(service.Route(TaskKind::Compact).backend == rebuilt);  // 新指纹下稳定

    // 指纹外字段(切端推理档位,不影响后端连接)不改指纹、不误伤缓存。
    const std::string fingerprint_stable = lubancode::config::ProviderConnectionFingerprint(*entry_b);
    entry_b->model_reasoning_effort = "high";
    CHECK(lubancode::config::ProviderConnectionFingerprint(*entry_b) == fingerprint_stable);
    CHECK(service.Route(TaskKind::Compact).backend == rebuilt);

    // 删除目标端(/provider remove 落盘成功后的形状):不再交出旧 client
    // 发新请求——缓存查询不得先于条目存在性。
    auto& providers = result.config.providers;
    providers.erase(std::remove_if(providers.begin(), providers.end(),
                                   [](const lubancode::config::ProviderConfig& provider) {
                                       return provider.name == "targetB";
                                   }),
                    providers.end());
    CHECK(service.Route(TaskKind::Compact).backend == nullptr);
    auto detached = service.RouteDetached(TaskKind::SessionTitle);
    CHECK(detached.backend == nullptr);  // 独占路同样不再建
}
