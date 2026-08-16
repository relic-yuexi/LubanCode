// 模型分工第一期的单测:三角色解析与回退链、任务映射、compact_model
// 兼容别名、usage 分角色记账、/model roles 短表与配置解析(规格"测试"
// 节:三角色解析、配置优先级和 cheap/lao -> normal 回退、compact_model
// 旧配置的兼容与冲突提示、usage 分角色记账)。
#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <string>

#include "agent/model_router.hpp"
#include "app/model_router.hpp"
#include "app/session_title.hpp"
#include "config/config.hpp"

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
        lubancode::config::LubancodeEnvValues{}, std::optional<lubancode::config::FileConfig>{*parsed},
        lubancode::config::GenericEnvValues{});
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
    CHECK(DefaultRoleForTask(TaskKind::Microcompact) == ModelRole::Cheap);
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
    REQUIRE(lines.size() == 2);
    // 顺序固定 cheap -> normal,好对照 /model roles。
    CHECK(lines[0].find("cheap") != std::string::npos);
    CHECK(lines[0].find("m-cheap") != std::string::npos);
    CHECK(lines[0].find("2 次调用") != std::string::npos);
    CHECK(lines[0].find("2000") != std::string::npos);  // 输入累计
    CHECK(lines[1].find("usage 未报告") != std::string::npos);

    ledger.RecordFallback(TaskKind::Compact, ModelRole::Cheap, ModelRole::Normal, "provider 超时");
    REQUIRE(ledger.fallback_notes().size() == 1);
    CHECK(ledger.fallback_notes()[0].find("cheap") != std::string::npos);
    CHECK(ledger.fallback_notes()[0].find("normal") != std::string::npos);
    // from == to 不是回退,不记。
    ledger.RecordFallback(TaskKind::Compact, ModelRole::Normal, ModelRole::Normal, "x");
    CHECK(ledger.fallback_notes().size() == 1);
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
    }
}
