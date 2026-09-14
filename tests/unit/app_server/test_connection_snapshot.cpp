// 应用Worker接入单 §八(本单切片)+ §四.108/111 复查的验收册:
//
//   连接快照(connection_snapshot.hpp 纯函数件)
//     - MaskEndpointUrl:路径/查询/userinfo 剥掉,只留 scheme://host[:port];
//       没 scheme 的写法宁可空着不记原文。
//     - DescribeSecretReference:只说钥匙在哪(env 变量名/provider key_env/
//       inline/none),不说钥匙是什么;环境变量优先级压过 provider 时按变量名记。
//     - ConnectionConfigVersion:非敏感连接事实的 sha256——换钥匙版本不动
//       (§八 177"不记录可离线猜测密钥的内容哈希"的可验面:料里没钥匙,
//       拿版本反推不出钥匙),换模型/端点版本动。
//     - FreezeConnectionSnapshot:wire/model/provider/endpoint/secretRef/
//       configVersion/sources/roles 全家福;角色路由三态(off/shorthand/
//       advanced)与 compact;整份 dump 零密钥。
//   单 Worker 连接冻结(§八 178,server 层)
//     - 同一 Server 进程内逐场 thread 拿同一份冻结合同;运行期改环境变量
//       不影响本进程(配置在启动时读一次,快照在 options 里冻结)。
//     - 未递快照(老注入形态)thread/started 不带 connection 字段,行为不变。
//   脱敏 effective-config(§四.111)
//     - 根路径/来源清单/取值来源/部署档指纹/功能状态各就各位;零密钥。
//   托管裁剪复查(§四.108,逐项补钉)
//     - 个人魂(SOUL.md)/法(system_prompt.md)/个人 config 的 soul 字段、
//       cwd 的 AGENTS.md,在应用根语义的 headless 装配路一个都不进——来源
//       根只认材料根(cli_app 生产接线的同一折法),提示组合不读个人文件。
//       Hook 的自动加载口在 app-server 路无代码路径(源码级证据见单子
//       记账),测试钉的是"Soul/法/AGENTS 三张自动加载的嘴"。
#include <doctest/doctest.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>  // SetEnvironmentVariableW:EnvGuard 的 Win32 面
#endif

#include <nlohmann/json.hpp>

#include "agent/agent_definition.hpp"
#include "api/backend.hpp"
#include "app_server/agent_wiring.hpp"
#include "app_server/connection.hpp"
#include "app_server/connection_snapshot.hpp"
#include "app_server/server.hpp"
#include "config/config.hpp"
#include "platform/paths.hpp"
#include "tools/path_utils.hpp"

namespace fs = std::filesystem;

using namespace lubancode;
using namespace lubancode::app_server;

namespace {

// test_runtime_paths 同款双写 EnvGuard:值非空 CRT+Win32 都设(生产
// HomeLubancodeDir/GetEnvVarPresent 走 Win32 面);析构两面都删。
struct EnvGuard {
    explicit EnvGuard(const char* name, const std::string& value) : name_(name) {
#ifdef _WIN32
        _putenv((std::string(name_) + "=" + value).c_str());
        SetEnvironmentVariableW(lubancode::platform::Utf8ToWide(name_).c_str(),
                                lubancode::platform::Utf8ToWide(value).c_str());
#else
        setenv(name_, value.c_str(), 1);
#endif
    }
    ~EnvGuard() {
#ifdef _WIN32
        _putenv((std::string(name_) + "=").c_str());
        SetEnvironmentVariableW(lubancode::platform::Utf8ToWide(name_).c_str(), nullptr);
#else
        unsetenv(name_);
#endif
    }
    const char* name_;
};

fs::path FreshRoot(const char* tag) {
    const auto dir = fs::temp_directory_path() / ("lubancode-conn-snapshot-" + std::string(tag));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

void WriteFile(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << content;
}

class StubBackend : public api::Backend {
public:
    std::expected<void, api::Error> send_stream(const api::Request&,
                                                const std::function<void(const api::StreamEvent&)>&,
                                                const std::atomic<bool>* = nullptr) override {
        return {};
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// MaskEndpointUrl
// ---------------------------------------------------------------------------

TEST_CASE("连接快照:端点脱敏——路径/查询/userinfo 一概剥掉") {
    CHECK(MaskEndpointUrl("https://models.example.com/v1/path?token=secret") ==
          "https://models.example.com");
    CHECK(MaskEndpointUrl("http://127.0.0.1:8080/api") == "http://127.0.0.1:8080");
    CHECK(MaskEndpointUrl("https://user:pass@api.example.com/x#frag") == "https://api.example.com");
    CHECK(MaskEndpointUrl("https://api.example.com") == "https://api.example.com");
    // 没 scheme 段:宁可空着(快照 endpoint 落 null),不冒险记原文。
    CHECK(MaskEndpointUrl("api.example.com/v1").empty());
    CHECK(MaskEndpointUrl("").empty());
    CHECK(MaskEndpointUrl("://weird").empty());
}

// ---------------------------------------------------------------------------
// DescribeSecretReference
// ---------------------------------------------------------------------------

TEST_CASE("连接快照:密钥引用只说在哪——env 变量名/provider 声明/inline/none") {
    config::ConfigSources env_sources;
    env_sources.auth_token = config::Source::LubanEnv;
    CHECK(DescribeSecretReference(config::Config{}, env_sources) == "env:LUBAN_API_KEY");
    env_sources.auth_token = config::Source::LubancodeEnv;
    CHECK(DescribeSecretReference(config::Config{}, env_sources) == "env:LUBANCODE_API_KEY");

    // provider 生效:按 provider 声明面描述,不提值。
    config::Config with_provider;
    config::ProviderConfig provider;
    provider.name = "acme";
    provider.auth = config::ProviderAuthMode::Env;
    provider.key_env = "ACME_API_TOKEN";
    with_provider.providers = {provider};
    with_provider.active_provider = "acme";
    config::ConfigSources provider_sources;
    provider_sources.auth_token = config::Source::GlobalConfigFile;
    with_provider.auth_mode = config::ProviderAuthMode::Env;
    CHECK(DescribeSecretReference(with_provider, provider_sources) ==
          "provider:acme:env:ACME_API_TOKEN");
    with_provider.auth_mode = config::ProviderAuthMode::Inline;
    provider.auth = config::ProviderAuthMode::Inline;
    with_provider.providers = {provider};
    CHECK(DescribeSecretReference(with_provider, provider_sources) == "provider:acme:inline");
    with_provider.auth_mode = config::ProviderAuthMode::None;
    provider.auth = config::ProviderAuthMode::None;
    with_provider.providers = {provider};
    CHECK(DescribeSecretReference(with_provider, provider_sources) == "provider:acme:none");

    // 环境变量压过 provider 时按变量名记(sources 还停在 env 级)。
    config::ConfigSources override_sources;
    override_sources.auth_token = config::Source::LubanEnv;
    CHECK(DescribeSecretReference(with_provider, override_sources) == "env:LUBAN_API_KEY");

    // 配置文件兜底与空缺。
    config::ConfigSources file_sources;
    file_sources.auth_token = config::Source::ProjectConfigFile;
    CHECK(DescribeSecretReference(config::Config{}, file_sources) == "config-file:inline");
    CHECK(DescribeSecretReference(config::Config{}, config::ConfigSources{}) == "none");
}

// ---------------------------------------------------------------------------
// ConnectionConfigVersion:换钥匙不动、换模型/端点动
// ---------------------------------------------------------------------------

TEST_CASE("连接快照:配置版本跟非敏感事实——换钥匙不动,换模型/端点动") {
    config::Config base;
    base.wire = config::Wire::ChatCompletions;
    base.base_url = "https://models.example.com";
    base.model = "demo-model";
    base.auth_token = "FAKE_KEY_ONE";

    config::Config rotated = base;
    rotated.auth_token = "FAKE_KEY_TWO";  // 钥匙轮换:版本不动(料里没钥匙)
    CHECK(ConnectionConfigVersion(rotated) == ConnectionConfigVersion(base));

    config::Config other_model = base;
    other_model.model = "other-model";
    CHECK(ConnectionConfigVersion(other_model) != ConnectionConfigVersion(base));

    config::Config other_endpoint = base;
    other_endpoint.base_url = "https://other.example.com";
    CHECK(ConnectionConfigVersion(other_endpoint) != ConnectionConfigVersion(base));

    // 角色路由也是连接事实:开 shorthand 改版本。
    config::Config with_role = base;
    with_role.cheap_model = "cheap-model";
    CHECK(ConnectionConfigVersion(with_role) != ConnectionConfigVersion(base));
}

// ---------------------------------------------------------------------------
// FreezeConnectionSnapshot:全家福 + 零密钥
// ---------------------------------------------------------------------------

TEST_CASE("连接快照:冻结件全家福——wire 规范名/来源标注/角色路由/零密钥") {
    config::Config config;
    config.wire = config::Wire::ChatCompletions;
    config.base_url = "https://models.example.com/v1?token=FAKE_URL_KEY";
    config.model = "demo-model";
    config.auth_token = "FAKE_SECRET_VALUE_XYZ";
    config.compact_model = "compact-model";
    config.cheap_model = "cheap-model";

    config::ConfigSources sources;
    sources.wire = config::Source::LubancodeEnv;
    sources.base_url = config::Source::LubanEnv;
    sources.model = config::Source::ProjectConfigFile;
    sources.auth_token = config::Source::LubanEnv;
    sources.compact_model = config::Source::GlobalConfigFile;
    sources.cheap_model = config::Source::GlobalConfigFile;

    const nlohmann::json snapshot = FreezeConnectionSnapshot(config, sources);

    CHECK(snapshot["wire"] == "openai-chat-completions");  // 规范名,不是旧别名
    CHECK(snapshot["model"] == "demo-model");
    CHECK(snapshot["endpoint"] == "https://models.example.com");  // 路径/查询剥掉
    CHECK(snapshot["secretRef"] == "env:LUBAN_API_KEY");
    CHECK(snapshot["configVersion"] == ConnectionConfigVersion(config));
    CHECK(snapshot["sources"]["wire"] == config::ToString(config::Source::LubancodeEnv));
    CHECK(snapshot["sources"]["model"] == config::ToString(config::Source::ProjectConfigFile));
    // 角色路由:shorthand 档,各角色带来源;normal/lao 未配置落 null。
    CHECK(snapshot["roles"]["routing"] == "shorthand");
    CHECK(snapshot["roles"]["cheap"]["model"] == "cheap-model");
    CHECK(snapshot["roles"]["compact"]["model"] == "compact-model");
    CHECK(snapshot["roles"]["normal"].is_null());
    CHECK(snapshot["roles"]["lao"].is_null());

    // 零密钥红线:整份 dump 不见钥匙正文,也不见 base_url 的查询段。
    const std::string dumped = snapshot.dump();
    CHECK(dumped.find("FAKE_SECRET_VALUE_XYZ") == std::string::npos);
    CHECK(dumped.find("FAKE_URL_KEY") == std::string::npos);
    CHECK(dumped.find("/v1") == std::string::npos);

    // advanced 段:model_roles 非空即 advanced(压过 shorthand 字段)。
    config::Config advanced = config;
    advanced.model_roles.normal.model = "role-model";
    const nlohmann::json advanced_snapshot =
        FreezeConnectionSnapshot(advanced, config::ConfigSources{});
    CHECK(advanced_snapshot["roles"]["routing"] == "advanced");
    CHECK(advanced_snapshot["roles"]["normal"]["model"] == "role-model");
}

TEST_CASE("连接快照:端点未配置落 null,不记空串冒充") {
    config::Config config;
    config.model = "demo-model";
    config.wire = config::Wire::Anthropic;
    const nlohmann::json snapshot = FreezeConnectionSnapshot(config, config::ConfigSources{});
    CHECK(snapshot["endpoint"].is_null());
    CHECK(snapshot["provider"].is_string());
}

// ---------------------------------------------------------------------------
// 单 Worker 连接冻结(server 层,§八 178)
// ---------------------------------------------------------------------------

TEST_CASE("单 Worker 连接冻结:同进程逐场同一份,改 env 不影响,未冻结不带字段") {
    config::Config frozen_config;
    frozen_config.wire = config::Wire::Anthropic;
    frozen_config.model = "frozen-model";
    const nlohmann::json frozen =
        FreezeConnectionSnapshot(frozen_config, config::ConfigSources{});

    // "Worker 进程":启动时冻结一份,进程期内不重读(测试用 EnvGuard 改
    // 环境变量模拟"运行时换配置"——快照在 options 里,环境改了它也不动)。
    // workspaces 根指临时目录:会话账落盘走真路,但不碰真机用户目录。
    EnvGuard late_model("LUBAN_MODEL", "changed-after-start");
    const fs::path sessions = FreshRoot("freeze-sessions");
    app_server::ServerOptions options;
    options.workspaces_dir = lubancode::platform::PathToUtf8(sessions / "workspaces");
    options.cwd = "/test/cwd";
    options.connection_snapshot = frozen;
    app_server::Server server(std::move(options),
                              []() -> std::unique_ptr<api::Backend> {
                                  return std::make_unique<StubBackend>();
                              },
                              nullptr);
    auto dispatcher = std::make_shared<app_server::Dispatcher>();
    server.AttachForTest(std::make_unique<app_server::StdioConnection>(
        std::move(dispatcher), [](const std::string&) {}, []() { return std::string(); }, 256));

    std::string error_code;
    const nlohmann::json first = server.HandleThreadStart(nlohmann::json::object(), error_code);
    REQUIRE(error_code.empty());
    REQUIRE(first.contains("connection"));
    CHECK(first["connection"] == frozen);  // 逐字节同一份冻结合同

    // 第二场:环境变量已经改了,快照纹丝不动(只影响新起的 Worker 进程)。
    // cwd 换一枚(不同 workspace 键):第一场的账本还持着原 workspace 的柄,
    // 分开落各查各的,不碰锁语义。
    const nlohmann::json second =
        server.HandleThreadStart(nlohmann::json{{"cwd", "/test/cwd-2"}}, error_code);
    REQUIRE(error_code.empty());
    REQUIRE(second.contains("connection"));
    CHECK(second["connection"] == frozen);
    CHECK(second["connection"]["model"] == "frozen-model");

    // 老注入形态(未递快照):不带 connection 字段,回执形状零变化。
    // 独立 workspaces 根:第一台的场没停,账本还持着那只 workspace 的柄,
    // 撞同一 workspace 键会吃锁——分开落,各查各的。
    app_server::ServerOptions bare_options;
    bare_options.workspaces_dir =
        lubancode::platform::PathToUtf8(FreshRoot("freeze-sessions-bare") / "workspaces");
    bare_options.cwd = "/test/cwd-bare";
    app_server::Server bare(std::move(bare_options),
                            []() -> std::unique_ptr<api::Backend> {
                                return std::make_unique<StubBackend>();
                            },
                            nullptr);
    auto bare_dispatcher = std::make_shared<app_server::Dispatcher>();
    bare.AttachForTest(std::make_unique<app_server::StdioConnection>(
        std::move(bare_dispatcher), [](const std::string&) {}, []() { return std::string(); },
        256));
    const nlohmann::json legacy = bare.HandleThreadStart(nlohmann::json::object(), error_code);
    REQUIRE(error_code.empty());
    CHECK_FALSE(legacy.contains("connection"));
}

// ---------------------------------------------------------------------------
// 脱敏 effective-config(§四.111)
// ---------------------------------------------------------------------------

TEST_CASE("effective-config 诊断:根路径/来源/取值来源/部署档/功能状态,零密钥") {
    config::Config config;
    config.wire = config::Wire::ChatCompletions;
    config.base_url = "https://models.example.com/v1";
    config.model = "demo-model";
    config.auth_token = "FAKE_SECRET_DIAG";
    config.compact_model = "compact-model";

    config::ConfigSources sources;
    sources.model = config::Source::LubanEnv;

    HarnessProfile harness;
    harness.name = "diag";
    harness.features_enabled.insert("mcp");
    harness.features_enabled.insert("plugins");
    harness.tools.mode = HarnessToolPolicy::Mode::Only;
    harness.tools.allow = {"plugin__demo__search"};

    EffectiveConfigInput input;
    input.config = &config;
    input.sources = &sources;
    input.config_root = "D:/app/config";
    input.data_root = "D:/app/state";
    input.managed = true;
    input.app_root_active = true;
    input.global_config_path = "D:/app/config/config.json";
    input.profile_path = "D:/app/config/deployment.json";
    input.profile_sha256 = "abc123";
    input.harness = &harness;

    const std::string text = FormatEffectiveConfigDiagnostics(input);
    CHECK(text.find("参数根=D:/app/config") != std::string::npos);
    CHECK(text.find("数据根=D:/app/state") != std::string::npos);
    CHECK(text.find("托管=1") != std::string::npos);
    CHECK(text.find("全局=D:/app/config/config.json") != std::string::npos);
    // 取值来源:model 来自 LUBAN_MODEL(env)——验证覆盖关系的口子。
    CHECK(text.find("model=demo-model(" + config::ToString(config::Source::LubanEnv) + ")") !=
          std::string::npos);
    CHECK(text.find("端点=https://models.example.com") != std::string::npos);
    CHECK(text.find("密钥引用=") != std::string::npos);
    CHECK(text.find("compact=compact-model") != std::string::npos);
    CHECK(text.find("部署档=D:/app/config/deployment.json") != std::string::npos);
    CHECK(text.find("sha256=abc123") != std::string::npos);
    CHECK(text.find("mcp=1") != std::string::npos);
    CHECK(text.find("plugins=1") != std::string::npos);
    CHECK(text.find("skills=0") != std::string::npos);
    // 零密钥:诊断文本不见钥匙正文,也不见端点路径段。
    CHECK(text.find("FAKE_SECRET_DIAG") == std::string::npos);
    CHECK(text.find("/v1") == std::string::npos);

    // 无档:显式零工具默认档,如实说(§2.3)。
    EffectiveConfigInput bare_input;
    bare_input.config = &config;
    bare_input.sources = &sources;
    const std::string bare_text = FormatEffectiveConfigDiagnostics(bare_input);
    CHECK(bare_text.find("部署档=无(显式零工具默认档)") != std::string::npos);
    CHECK(bare_text.find("参数根=(个人布局)") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 托管裁剪复查(§四.108):个人魂/法/AGENTS/个人 config 不进 headless 装配
// ---------------------------------------------------------------------------

TEST_CASE("托管裁剪复查:个人魂/法/AGENTS/个人 config 的标记不进 headless 提示") {
    const fs::path material_root = FreshRoot("trim-material");
    const fs::path personal_home = FreshRoot("trim-personal");
    const fs::path poisoned_cwd = FreshRoot("trim-cwd");

    // 布毒:个人家目录的魂与法、个人 config.json 的 soul 字段、cwd 的
    // AGENTS.md,各带唯一标记;材料根(参数根)保持干净。
    WriteFile(personal_home / ".lubancode" / "SOUL.md", "PERSONAL-SOUL-MARKER");
    WriteFile(personal_home / ".lubancode" / "system_prompt.md", "PERSONAL-LAW-MARKER");
    WriteFile(personal_home / ".lubancode" / "config.json",
              R"json({"soul": "evil-soul", "api_key": "FAKE_PERSONAL_KEY",
                      "system_prompt_file": "SOUL.md"})json");
    WriteFile(poisoned_cwd / "AGENTS.md", "PERSONAL-AGENTS-MARKER");
    WriteFile(material_root / ".keep", "");

    // 应用根语义:参数根指向干净材料根,个人主目录指向毒树——生产接线
    // (cli_app RunAppServerMode)的同一折法:来源只从 HomeLubancodeDir()
    // (应用根语义下即参数根)拼,绝不从个人家目录或 cwd 拼。
    EnvGuard home("LUBANCODE_HOME", lubancode::platform::PathToUtf8(material_root));
#ifdef _WIN32
    EnvGuard user_profile("USERPROFILE", lubancode::platform::PathToUtf8(personal_home));
#else
    EnvGuard home_env("HOME", lubancode::platform::PathToUtf8(personal_home));
#endif
    const auto material = config::HomeLubancodeDir();
    REQUIRE(material.has_value());
    CHECK(*material == lubancode::platform::PathToUtf8(material_root));

    // cli_app 生产接线同款折法:材料根(UTF-8)折 fs::path 再拼三处。
    const fs::path material_path = tools::Utf8ToPath(*material);
    HarnessAgentSources sources;
    sources.agents_dir = material_path / "agents";  // 不存在:无档案,合法(不点名)
    sources.skills_dir = material_path / "skills";
    sources.prompts_dir_utf8 = *material + "/prompts";

    HarnessProfile harness;  // 不点名 agentRef:persona 空,core 落发行内置
    const HarnessAgentPlanResult plan =
        ResolveHarnessAgentPlan(harness, std::move(sources), "anthropic",
                                lubancode::platform::PathToUtf8(poisoned_cwd));
    REQUIRE(plan.plan.has_value());

    HarnessPromptInput prompt_input;
    prompt_input.plan = &*plan.plan;
    const HarnessPromptResult composed = ComposeHarnessSystemPrompt(prompt_input);
    REQUIRE(composed.error.empty());

    // 三张自动加载的嘴全钉死:魂(SOUL.md/souls/)、法(system_prompt.md/
    // --system-prompt)、项目指令(AGENTS.md/cwd)——headless 装配一个不读,
    // 个人 config 的 soul 字段也不生效(Soul 进 headless 只有部署档显式
    // 点名一条路,当前未接,如实为空)。
    CHECK(composed.text.find("PERSONAL-SOUL-MARKER") == std::string::npos);
    CHECK(composed.text.find("PERSONAL-LAW-MARKER") == std::string::npos);
    CHECK(composed.text.find("PERSONAL-AGENTS-MARKER") == std::string::npos);
    CHECK(composed.text.find("evil-soul") == std::string::npos);
    CHECK(composed.text.find("风格叠加层") == std::string::npos);  // 魂段整个不在
    CHECK(composed.text.find("FAKE_PERSONAL_KEY") == std::string::npos);
}
