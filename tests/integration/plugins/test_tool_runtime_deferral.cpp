// 用显式插件根验 ToolRuntime 的延迟挂载与 token 预算门。
// 每场独占临时目录，不读取或修改 HOME、USERPROFILE、进程 cwd。
// process 插件只解析清单；此册不调用插件，不启动模型服务。

#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "agent/context.hpp"  // EstimateUtf8Tokens:延迟本金与生产同一把尺
#include "api/backend.hpp"
#include "app/tool_runtime.hpp"
#include "platform/paths.hpp"
#include "tool_assembly_fixture.hpp"

namespace {

class NullBackend : public lubancode::api::Backend {
public:
    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request& request,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        (void)request;
        (void)on_event;
        (void)cancel;
        return {};
    }
};

lubancode::config::Config EmptyConfig() {
    lubancode::config::Config config;
    config.mcp_servers.clear();
    config.lsp_servers.clear();
    config.search = lubancode::config::SearchConfig();
    return config;
}

const std::vector<lubancode::tools::SkillMeta>& NoSkills() {
    static const std::vector<lubancode::tools::SkillMeta> skills;
    return skills;
}

using lubancode::test_support::ToolAssemblyFixture;

// 往显式 plugins 根装 count 枚 process 插件,每枚声明
// tools_per_plugin 件工具。工具名带插件序号,跨插件不重名(重名会被
// ScanPluginDirectories 整件拒掉)。description 可调:预算门册要拿"描述
// 薄/肥"两种本金形状对账(P4),枚数册用默认薄描述。
void InstallPlugins(const std::filesystem::path& plugins, int count, int tools_per_plugin,
                          const std::string& description = "deferral 对账用的占位工具") {
    for (int i = 0; i < count; ++i) {
        const std::filesystem::path dir = plugins / ("defl_" + std::to_string(i));
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::string tools_json;
        for (int t = 0; t < tools_per_plugin; ++t) {
            tools_json += (t > 0 ? "," : "");
            tools_json += "{\"name\": \"t" + std::to_string(t) +
                          "\", \"description\": \"" + description +
                          "\", "
                          "\"input_schema\": {\"type\": \"object\"}}";
        }
        // command 填 "echo":装配只解析 manifest 不起进程,这一格不会被
        // 执行;相对可执行名也不触发 ${plugin_dir} 的路径圈禁。
        const std::string manifest = "{\"manifest_version\": 1, \"id\": \"defl_" + std::to_string(i) +
                                     "\", \"version\": \"1.0.0\", \"language\": \"shell\", "
                                     "\"runtime\": {\"kind\": \"process\", \"command\": \"echo\"}, "
                                     "\"tools\": [" +
                                     tools_json + "]}";
        std::ofstream out(dir / "plugin.json", std::ios::binary);
        out << manifest;
    }
}

// 一轮三档共用:装 count 枚、构造、还账。
void CheckDeferralForPluginCount(int count) {
    CAPTURE(count);
    ToolAssemblyFixture fixture;
    InstallPlugins(fixture.Plugins(), count, /*tools_per_plugin=*/3);
    lubancode::config::Config config = EmptyConfig();
    // 动态工具 P4:启用判定从"枚数门"一道改成"枚数门 + token 预算门"
    // 双闸(ShouldDeferTools)。本册对账的是枚数门那一道——预算门显式关
    // 掉(floor=0,只看枚数,与 P4 之前的合同同口径),免得两道闸的账搅
    // 在一本册里;预算门的产品行为在下面那册单独对账。
    config.tool_search_token_floor = 0;
    NullBackend backend;
    lubancode::app::ToolRuntime runtime(config, backend, NoSkills(),
                                         /*skills_segment=*/"", fixture.Plan(),
                                         lubancode::app::ToolRuntime::Options{});

    // 插件真挂上了(manifest 一枚不落),不是空转的断言。
    REQUIRE(runtime.process_manifests().size() == static_cast<std::size_t>(count));

    const auto threshold = static_cast<std::size_t>(config.tool_search_threshold);  // 默认 20
    const std::size_t main_size = runtime.main_registry().All().size();
    const std::size_t sub_size = runtime.sub_registry().All().size();
    INFO("main tools: ", main_size, " sub tools: ", sub_size, " threshold: ", threshold);
    // 口径钉死:总数(不含 tool_search 自身,装配在注册它之前数的)严格
    // 大于阈值才启用——DeferralEnabled 的合同,主表子表同一条。
    CHECK((main_size > threshold) == runtime.main_deferral());
    CHECK((sub_size > threshold) == runtime.sub_deferral());

    if (count >= 10) {
        // 十枚 × 三工具 = 30 枚插件工具,加上内置一批,两张表都压过线:
        // 产品行为对账——全局插件多了,延迟挂载真的开。
        CHECK(runtime.main_deferral());
        CHECK(runtime.sub_deferral());
        CHECK(runtime.main_registry().Find("tool_search") != nullptr);
        CHECK(runtime.sub_registry().Find("tool_search") != nullptr);
        // 插件工具在表里、标了 deferred,过滤器把它拦在 tools 数组外;
        // 内置工具不 deferred,照旧放行——这就是延迟挂载的分界线。
        lubancode::tools::Tool* plugin_tool = runtime.main_registry().Find("plugin__defl_1__t0");
        REQUIRE(plugin_tool != nullptr);
        CHECK(plugin_tool->deferred());
        CHECK_FALSE(runtime.main_tool_filter()(*plugin_tool));
        CHECK(runtime.main_tool_filter()(*runtime.main_registry().Find("read_file")));
    } else {
        // 0 枚与 1 枚(三工具)都还在线下:延迟不启用,tool_search 不挂,
        // 与单测册的口径一致——装得少不改变直挂行为。
        CHECK_FALSE(runtime.main_deferral());
        CHECK_FALSE(runtime.sub_deferral());
        CHECK(runtime.main_registry().Find("tool_search") == nullptr);
        CHECK(runtime.sub_registry().Find("tool_search") == nullptr);
        CHECK(runtime.main_tool_filter()(*runtime.main_registry().Find("read_file")));
    }
}

}  // namespace

using namespace lubancode::app;

TEST_CASE("全局插件数与 tool deferral:0、1 枚不触发,10 枚越线触发") {
    CheckDeferralForPluginCount(0);
    CheckDeferralForPluginCount(1);
    CheckDeferralForPluginCount(10);
}

// ---------------------------------------------------------------------------
// 动态工具 PromptCache 守恒单 P1:配置开 proxy_reference 后的装配落位。
// 越线 + proxy 档:tool_search/tool_invoke 双双常驻两表、两侧 resolver 各
// 一只(scope 不串)、执行资格与 exposure 分家(deferred 工具按名不进
// 顶层、经 tool_invoke 的执行资格放行);disabled 档压过阈值强制全量;
// 不写(默认)2026-09-03 切默认后经推荐档落 proxy(与显式同款);显式写
// legacy_expand 才是兼容档(无 tool_invoke)。这是"配置 → 装配"那一拍的
// 对账,发现/调用/前缀三拍在单测册(test_request_prefix/test_loop)钉。
// ---------------------------------------------------------------------------
TEST_CASE("deferred_tool_mode=proxy_reference: 装配落位——双壳常驻、双 resolver、闸分家") {
    ToolAssemblyFixture fixture;
    InstallPlugins(fixture.Plugins(), /*count=*/10, /*tools_per_plugin=*/3);  // 30 枚插件工具,越线
    NullBackend backend;

    // ---- proxy 档 ----
    {
        lubancode::config::Config config = EmptyConfig();
        // 本册对账"模式落位",不管启用门槛:预算门关掉(floor=0),30 枚
        // 薄插件只按枚数门越线,deferral 起来后模式装配才有得验。
        config.tool_search_token_floor = 0;
        config.deferred_tool_mode = "proxy_reference";
        ToolRuntime runtime(config, backend, NoSkills(),
                            /*skills_segment=*/"", fixture.Plan(), ToolRuntime::Options{});
        CHECK(runtime.main_deferral());
        CHECK(runtime.main_proxy_enabled());
        CHECK(runtime.sub_proxy_enabled());
        CHECK(runtime.main_tool_mode() == lubancode::tools::DeferredToolMode::ProxyReference);

        // 双壳常驻两表,tool_invoke 紧跟 tool_search(注册次序即顶层 tools 次序)。
        for (lubancode::tools::ToolRegistry* registry : {&runtime.main_registry(), &runtime.sub_registry()}) {
            REQUIRE(registry->Find("tool_search") != nullptr);
            REQUIRE(registry->Find("tool_invoke") != nullptr);
            int search_at = -1;
            int invoke_at = -1;
            const auto& all = registry->All();
            for (std::size_t i = 0; i < all.size(); ++i) {
                if (all[i]->name() == "tool_search") search_at = static_cast<int>(i);
                if (all[i]->name() == "tool_invoke") invoke_at = static_cast<int>(i);
            }
            CHECK(invoke_at == search_at + 1);
        }

        // 两侧 resolver 各一只,scope 分明(main 的 ref 进不了 sub 的账)。
        REQUIRE(runtime.main_tool_ref_resolver() != nullptr);
        REQUIRE(runtime.sub_tool_ref_resolver() != nullptr);
        CHECK(runtime.main_tool_ref_resolver()->session_scope() == "main");
        CHECK(runtime.sub_tool_ref_resolver()->session_scope() == "sub");

        // 闸分家:exposure 过滤把延迟工具拦在顶层外;执行资格(经 tool_invoke
        // 解引用来的调用)放行——发现不等于授权,授权也不等于直名可调。
        lubancode::tools::Tool* plugin_tool = runtime.main_registry().Find("plugin__defl_1__t0");
        REQUIRE(plugin_tool != nullptr);
        CHECK(plugin_tool->deferred());
        CHECK_FALSE(runtime.main_tool_filter()(*plugin_tool));
        REQUIRE(runtime.main_execution_policy() != nullptr);
        CHECK(runtime.main_execution_policy()(*plugin_tool));
        CHECK(runtime.main_execution_denial().find(lubancode::tools::kErrToolRefNotAllowed) !=
              std::string::npos);
    }

    // ---- disabled 档:压过阈值,强制全量常驻 ----
    {
        lubancode::config::Config config = EmptyConfig();
        config.tool_search_token_floor = 0;  // 同上:预算门关掉,让"压过"有得验
        config.deferred_tool_mode = "disabled";
        ToolRuntime runtime(config, backend, NoSkills(),
                            /*skills_segment=*/"", fixture.Plan(), ToolRuntime::Options{});
        CHECK_FALSE(runtime.main_deferral());
        CHECK_FALSE(runtime.sub_deferral());
        CHECK(runtime.main_registry().Find("tool_search") == nullptr);
        CHECK(runtime.main_registry().Find("tool_invoke") == nullptr);
        // 全量常驻:延迟工具照样进顶层(没有延迟这回事了)。
        lubancode::tools::Tool* plugin_tool = runtime.main_registry().Find("plugin__defl_1__t0");
        REQUIRE(plugin_tool != nullptr);
        CHECK(runtime.main_tool_filter()(*plugin_tool));
    }

    // ---- 默认(不写)= 推荐档:2026-09-03 真机质量对照过门后切默认,空串
    // 在直构路落 kRecommendedDeferredToolMode(= proxy_reference)——与上面
    // 显式写 proxy_reference 同款落位。legacy 只剩显式写才生效。 ----
    {
        lubancode::config::Config config = EmptyConfig();
        config.tool_search_token_floor = 0;  // 同上:模式落位册,启用门槛只留枚数门
        ToolRuntime runtime(config, backend, NoSkills(),
                            /*skills_segment=*/"", fixture.Plan(), ToolRuntime::Options{});
        CHECK(runtime.main_deferral());
        CHECK(runtime.main_proxy_enabled());
        CHECK(runtime.main_registry().Find("tool_search") != nullptr);
        CHECK(runtime.main_registry().Find("tool_invoke") != nullptr);
        CHECK(runtime.main_tool_ref_resolver() != nullptr);
    }

    // ---- 显式写 legacy_expand 仍是兼容档:没有 tool_invoke,没有 resolver ----
    {
        lubancode::config::Config config = EmptyConfig();
        config.tool_search_token_floor = 0;
        config.deferred_tool_mode = "legacy_expand";
        ToolRuntime runtime(config, backend, NoSkills(),
                            /*skills_segment=*/"", fixture.Plan(), ToolRuntime::Options{});
        CHECK(runtime.main_deferral());
        CHECK_FALSE(runtime.main_proxy_enabled());
        CHECK(runtime.main_registry().Find("tool_search") != nullptr);
        CHECK(runtime.main_registry().Find("tool_invoke") == nullptr);
        CHECK(runtime.main_tool_ref_resolver() == nullptr);
    }

}

// ---------------------------------------------------------------------------
// 动态工具 P4·§十三 P4-1:token 预算门的产品行为册。P0 baseline 册实测
//(tests/unit/tools/test_tool_search.cpp "P0基线"):轻 schema/长描述形状
// 18 枚延迟本金约 1080 token,启用后首份请求 1151 > 全量常驻 1089,反赔;
// 重 schema/短描述形状本金约 2220 才有真省(634 < 2296)。默认 floor 1500
// 落在两实测点之间。这里用真插件链对账:同一批插件枚数都过线(30 枚 >
// 20),描述薄 -> 本金不够 -> 不启用;描述肥 -> 本金过线 -> 启用。配置写
// 0 可关掉这道门(回到 P4 之前的现状,上面那册的口径)。
// ---------------------------------------------------------------------------
TEST_CASE("P4 token 预算门: 枚数过了但延迟本金不够,不启用;本金过线才启用;floor=0 关门") {
    ToolAssemblyFixture thin_fixture;
    ToolAssemblyFixture fat_fixture;
    // 肥描述:55 个汉字,每枚描述约 82 token(EstimateUtf8Tokens 非ASCII
    // 1.5/字),加名字与 schema,30 枚本金约 2600 > 默认 floor 1500。
    const std::string fat_description =
        "远程服务里的示例只读工具,用于预算门场景的延迟本金测量,描述写得长一些才好把声明字节撑过默认预算线,"
        "模拟真实 MCP 工具里那种带用法说明、参数注意事项与错误码表的完整描述文本,再补一句占用说明凑足长度";
    InstallPlugins(thin_fixture.Plugins(), /*count=*/10, /*tools_per_plugin=*/3);  // 默认薄描述
    InstallPlugins(fat_fixture.Plugins(), /*count=*/10, /*tools_per_plugin=*/3, fat_description);
    NullBackend backend;

    // 延迟声明本金:与生产 DeferredDeclarationTokens 同一把尺(agent::
    // EstimateUtf8Tokens,名字+描述+schema),册里现量,断言不猜数。
    const auto deferred_principal = [](const lubancode::tools::ToolRegistry& registry) {
        std::size_t total = 0;
        for (const auto& tool : registry.All()) {
            if (!tool->deferred()) {
                continue;
            }
            total += lubancode::agent::EstimateUtf8Tokens(tool->name()) +
                     lubancode::agent::EstimateUtf8Tokens(tool->description()) +
                     lubancode::agent::EstimateUtf8Tokens(tool->input_schema().dump());
        }
        return total;
    };

    // 薄描述插件(默认描述)+ 默认配置(floor=1500):枚数门过了,本金没过
    // ——不启用,全量常驻(这就是 P4 重定的落点:从"枚数到就开"改成
    // "本金够才开")。
    {
        const auto& fixture = thin_fixture;
        lubancode::config::Config config = EmptyConfig();  // 默认 floor 1500
        ToolRuntime runtime(config, backend, NoSkills(),
                            /*skills_segment=*/"", fixture.Plan(), ToolRuntime::Options{});
        REQUIRE(runtime.process_manifests().size() == 10);
        const std::size_t thin_principal = deferred_principal(runtime.main_registry());
        CHECK(runtime.main_registry().All().size() >
              static_cast<std::size_t>(config.tool_search_threshold));  // 枚数门:开着
        CHECK(thin_principal < static_cast<std::size_t>(config.tool_search_token_floor));  // 本金:没过
        CHECK_FALSE(runtime.main_deferral());                                                // 预算门:拦下
        CHECK(runtime.main_registry().Find("tool_search") == nullptr);
        CHECK(runtime.main_registry().Find("tool_invoke") == nullptr);
        // 全量常驻:延迟工具照进顶层。
        lubancode::tools::Tool* plugin_tool = runtime.main_registry().Find("plugin__defl_1__t0");
        REQUIRE(plugin_tool != nullptr);
        CHECK(runtime.main_tool_filter()(*plugin_tool));
    }

    // 肥描述插件 + 同一份默认配置:本金过线,两道门都过,照旧启用。
    {
        const auto& fixture = fat_fixture;
        lubancode::config::Config config = EmptyConfig();
        ToolRuntime runtime(config, backend, NoSkills(),
                            /*skills_segment=*/"", fixture.Plan(), ToolRuntime::Options{});
        const std::size_t fat_principal = deferred_principal(runtime.main_registry());
        CHECK(fat_principal >= static_cast<std::size_t>(config.tool_search_token_floor));  // 本金:过了
        CHECK(runtime.main_deferral());
        CHECK(runtime.main_registry().Find("tool_search") != nullptr);
        CHECK(runtime.sub_deferral());  // 子表同一把尺
    }

    // 薄描述插件 + 显式 floor=0:预算门关掉,只看枚数(P4 之前的现状,
    // 用户可控回退)。
    {
        const auto& fixture = thin_fixture;
        lubancode::config::Config config = EmptyConfig();
        config.tool_search_token_floor = 0;
        ToolRuntime runtime(config, backend, NoSkills(),
                            /*skills_segment=*/"", fixture.Plan(), ToolRuntime::Options{});
        CHECK(runtime.main_deferral());
        CHECK(runtime.main_registry().Find("tool_search") != nullptr);
    }

}
