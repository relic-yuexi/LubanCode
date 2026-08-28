// Prompt Profile(自定义 Agent 与 Prompt Profile 单·阶段 2)的测试册:
// overlay 三层解析、拼装次序断言、PromptCapabilities、来源账本。
//
// 头一册是黄金基线:未选 Profile 时,default 拼装的完整输出必须与"嵌入
// 常量按契约 §6.2 次序逐段重构"的串逐字节相等。重构串与实现串两条独立
// 路径,相等即零 diff 的铁证——阶段 2 的任何改动都不许弄破它(单子验收
// 线"保住现有 default Prompt 输出")。

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "agent/prompt_assembler.hpp"
#include "agent/prompts.hpp"
#include "embedded_prompts.hpp"

using namespace lubancode::agent;

namespace {

// 黄金口径:固定 cwd/日期,开关全默认关(与 test_prompt_assembler 的
// BaseOptions 同款),不带 prompts_dir、不带 Profile。
PromptOptions GoldenOptions() {
    PromptOptions options;
    options.cwd = "D:/work";
    options.current_date = "2026-07-18";
    return options;
}

// 段与段之间 "\n\n" 相接——与 AssembleSystemPrompt 的 append 同一规矩。
std::string JoinSegments(const std::vector<std::string>& parts) {
    std::string out;
    for (const std::string& part : parts) {
        if (!out.empty()) {
            out += "\n\n";
        }
        out += part;
    }
    return out;
}

// 临时目录护栏:构造即建,析构即扫。三层 overlay 测试各造各的。
class TempDir {
public:
    explicit TempDir(const std::string& tag) {
        path_ = std::filesystem::temp_directory_path() /
                ("lubancode_prompt_profile_" + tag + "_" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    std::string Str() const { return path_.string(); }

    // 相对路径换 fs::path(删文件做"退层"验收用)。
    std::filesystem::path PathOf(const std::string& rel_path) const {
        return path_ / std::filesystem::path(rel_path);
    }

    void WriteModule(const std::string& rel_path, const std::string& content) {
        const std::filesystem::path full = PathOf(rel_path);
        std::filesystem::create_directories(full.parent_path());
        std::ofstream file(full, std::ios::binary | std::ios::trunc);
        file << content;
    }

private:
    std::filesystem::path path_;
};

bool Contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

// ---------------------------------------------------------------------------
// 黄金基线:default 拼装零 diff(单子阶段 2 验收线)
// ---------------------------------------------------------------------------

TEST_CASE("黄金基线:未选 Profile 的 default 拼装与逐段重构逐字节相等") {
    // 重构(只认嵌入常量与拼装次序,不走 AssembleSystemPrompt):
    //   core 模块(按 kAllModules 文件名序)-> 运行环境 -> 恒在四件套
    //   -> (skills/web/mcp/lsp/platform 开关全关,一个不注)
    //   -> modes/default.md(宿主内置,殿后)
    std::vector<std::string> parts;
    for (const auto& module : embedded::kAllModules) {
        if (std::string_view(module.rel_path).substr(0, 5) == "core/") {
            parts.emplace_back(module.content);
        }
    }
    REQUIRE(parts.size() == std::size(embedded::kCoreModules));
    parts.push_back(BuildEnvironmentSegment("D:/work", "2026-07-18"));
    parts.push_back(embedded::kFeature_files);
    parts.push_back(embedded::kFeature_shell);
    parts.push_back(embedded::kFeature_delegation);
    parts.push_back(embedded::kFeature_todo);
    parts.push_back(embedded::kMode_default);

    CHECK(AssembleSystemPrompt(GoldenOptions()) == JoinSegments(parts));
}

TEST_CASE("黄金基线:法(persona)替换 core 时,其余段次序与内容照旧") {
    std::vector<std::string> parts;
    parts.push_back("你是测试人格。");
    parts.push_back(BuildEnvironmentSegment("D:/work", "2026-07-18"));
    parts.push_back(embedded::kFeature_files);
    parts.push_back(embedded::kFeature_shell);
    parts.push_back(embedded::kFeature_delegation);
    parts.push_back(embedded::kFeature_todo);
    parts.push_back(embedded::kMode_default);

    PromptOptions options = GoldenOptions();
    options.persona = "你是测试人格。";
    CHECK(AssembleSystemPrompt(options) == JoinSegments(parts));
}

// ---------------------------------------------------------------------------
// 五层覆盖(契约 §6.2):内置 default -> 用户全局 default -> 内置 Profile
// -> 用户 Profile -> 项目 Profile。内置样例 Profile 是 browser-tester
// (core/10-identity.md + features/web.md,见 src/prompts/profiles/)。
// ---------------------------------------------------------------------------

// kProfileModules 里按相对路径取一条(找不到给空串,断言好写)。
std::string EmbeddedProfileText(const std::string& profile_rel) {
    for (const auto& module : embedded::kProfileModules) {
        if (profile_rel == module.rel_path) {
            return module.content;
        }
    }
    return std::string();
}

TEST_CASE("内置 Profile 表:browser-tester 样例在,且只点名自己的模块") {
    REQUIRE(std::size(embedded::kProfileModules) >= 2);
    CHECK(EmbeddedProfileText("profiles/browser-tester/core/10-identity.md").find("browser-tester") !=
          std::string::npos);
    CHECK(!EmbeddedProfileText("profiles/browser-tester/features/web.md").empty());
    // 稀疏:没点名 core/20-workflow.md。
    CHECK(EmbeddedProfileText("profiles/browser-tester/core/20-workflow.md").empty());
    // 内置 Profile 不进 kAllModules(default 树的播种/来源账不混 Profile)。
    for (const auto& module : embedded::kAllModules) {
        CHECK(std::string_view(module.rel_path).substr(0, 9) != "profiles/");
    }
}

TEST_CASE("内置 Profile 覆盖压过内置 default,只动点名的模块") {
    PromptOptions options = GoldenOptions();
    options.profile = "browser-tester";
    const std::string prompt = AssembleSystemPrompt(options);

    // 点名的模块换成了 Profile 版。
    CHECK(Contains(prompt, EmbeddedProfileText("profiles/browser-tester/core/10-identity.md").c_str()));
    // 没点名的 core 模块原样走内置 default。
    CHECK(Contains(prompt, embedded::kCoreModules[1]));
    CHECK(Contains(prompt, embedded::kCoreModules[2]));
    // 内置 default 的身份模块被压掉(被 Profile 版替换,不再出现)。
    CHECK_FALSE(Contains(prompt, embedded::kCoreModules[0]));
}

TEST_CASE("五层次序:用户全局 default 压内置 default,又被内置 Profile 压") {
    TempDir user("layers");
    user.WriteModule("core/10-identity.md", "用户全局的身份。");
    user.WriteModule("core/20-workflow.md", "用户全局的工作法。");

    // 不选 Profile:用户全局覆盖生效。
    PromptOptions plain = GoldenOptions();
    plain.prompts_dir = user.Str();
    std::string prompt = AssembleSystemPrompt(plain);
    CHECK(Contains(prompt, "用户全局的身份。"));
    CHECK(Contains(prompt, "用户全局的工作法。"));

    // 选中 browser-tester:其内置 Profile 覆盖 core/10-identity.md,压过用户
    // 全局;core/20-workflow.md 没被 Profile 点名,用户全局覆盖仍生效。
    PromptOptions profiled = GoldenOptions();
    profiled.prompts_dir = user.Str();
    profiled.profile = "browser-tester";
    prompt = AssembleSystemPrompt(profiled);
    CHECK_FALSE(Contains(prompt, "用户全局的身份。"));
    CHECK(Contains(prompt, EmbeddedProfileText("profiles/browser-tester/core/10-identity.md").c_str()));
    CHECK(Contains(prompt, "用户全局的工作法。"));
}

TEST_CASE("五层次序:用户 Profile 压内置 Profile,项目 Profile 压用户 Profile") {
    TempDir user("up");
    TempDir project("pp");
    user.WriteModule("profiles/browser-tester/core/10-identity.md", "用户层的 Profile 身份。");
    project.WriteModule("profiles/browser-tester/core/10-identity.md", "项目层的 Profile 身份。");
    project.WriteModule("profiles/browser-tester/features/todo.md", "项目层的待办方针。");

    PromptOptions options = GoldenOptions();
    options.prompts_dir = user.Str();
    options.project_prompts_dir = project.Str();
    options.profile = "browser-tester";
    const std::string prompt = AssembleSystemPrompt(options);

    // 项目层最重:core/10-identity.md 与 features/todo.md 都用项目层文本。
    CHECK(Contains(prompt, "项目层的 Profile 身份。"));
    CHECK(Contains(prompt, "项目层的待办方针。"));
    CHECK_FALSE(Contains(prompt, "用户层的 Profile 身份。"));
    CHECK_FALSE(Contains(prompt, EmbeddedProfileText("profiles/browser-tester/core/10-identity.md").c_str()));
    CHECK_FALSE(Contains(prompt, embedded::kFeature_todo));  // todo 模块也被项目层压掉
}

TEST_CASE("验收线:删掉项目层覆盖,稳稳退回用户层;再删退回内置层") {
    TempDir user("fallback");
    TempDir project("fallback-proj");
    user.WriteModule("profiles/browser-tester/features/web.md", "用户层的联网方针。");

    PromptOptions options = GoldenOptions();
    options.prompts_dir = user.Str();
    options.profile = "browser-tester";
    options.web = true;  // web feature 注入,web 模块的层替换才看得见

    // 只有用户层:用户层文本生效。
    std::string prompt = AssembleSystemPrompt(options);
    CHECK(Contains(prompt, "用户层的联网方针。"));

    // 项目层盖上来,再删掉——稳稳退回用户层。
    options.project_prompts_dir = project.Str();
    project.WriteModule("profiles/browser-tester/features/web.md", "项目层的联网方针。");
    prompt = AssembleSystemPrompt(options);
    CHECK(Contains(prompt, "项目层的联网方针。"));
    std::filesystem::remove(project.PathOf("profiles/browser-tester/features/web.md"));
    prompt = AssembleSystemPrompt(options);
    CHECK(Contains(prompt, "用户层的联网方针。"));

    // 用户层也删掉——退回内置 Profile 层。
    std::filesystem::remove(user.PathOf("profiles/browser-tester/features/web.md"));
    prompt = AssembleSystemPrompt(options);
    CHECK(Contains(prompt, EmbeddedProfileText("profiles/browser-tester/features/web.md").c_str()));
}

TEST_CASE("Profile 只影响点名 Agent:用户层放着别的 Profile 文件,default 输出零变化") {
    TempDir user("isolation");
    user.WriteModule("profiles/browser-tester/core/10-identity.md", "别人家 Profile 的身份。");
    user.WriteModule("profiles/another-agent/features/web.md", "又一个 Profile 的联网方针。");

    // default 上下文(不带 profile)的输出与黄金基线逐字节一致。
    PromptOptions options = GoldenOptions();
    options.prompts_dir = user.Str();
    std::vector<std::string> parts;
    for (const auto& module : embedded::kAllModules) {
        if (std::string_view(module.rel_path).substr(0, 5) == "core/") {
            parts.emplace_back(module.content);
        }
    }
    parts.push_back(BuildEnvironmentSegment("D:/work", "2026-07-18"));
    parts.push_back(embedded::kFeature_files);
    parts.push_back(embedded::kFeature_shell);
    parts.push_back(embedded::kFeature_delegation);
    parts.push_back(embedded::kFeature_todo);
    parts.push_back(embedded::kMode_default);
    CHECK(AssembleSystemPrompt(options) == JoinSegments(parts));
}

TEST_CASE("认不得的 Profile 名与显式 default:三层 Profile 都不参与") {
    TempDir user("unknown");
    // 用户层有别的 Profile 的文件,不得串味。
    user.WriteModule("profiles/another-agent/core/10-identity.md", "别人家 Profile 的身份。");

    PromptOptions options = GoldenOptions();
    options.prompts_dir = user.Str();
    options.profile = "ghost";  // 三层都没有这个名的目录
    std::string prompt = AssembleSystemPrompt(options);
    CHECK(prompt.find(AssembledDefaultPersona()) == 0);  // core 全走内置 default
    CHECK_FALSE(Contains(prompt, "别人家 Profile 的身份。"));

    // 显式 "default" = 强制用内置默认,不走 Profile 层(契约 §4.2)。
    options.profile = "default";
    prompt = AssembleSystemPrompt(options);
    CHECK(prompt.find(AssembledDefaultPersona()) == 0);
}

TEST_CASE("法(persona)替换 core:选了 Profile 也一样让位") {
    PromptOptions options = GoldenOptions();
    options.profile = "browser-tester";
    options.persona = "法说了算。";
    const std::string prompt = AssembleSystemPrompt(options);
    CHECK(prompt.find("法说了算。") == 0);
    CHECK_FALSE(Contains(prompt, EmbeddedProfileText("profiles/browser-tester/core/10-identity.md").c_str()));
    // 其余段照拼。
    CHECK(Contains(prompt, embedded::kFeature_files));
    CHECK(Contains(prompt, embedded::kMode_default));
}

TEST_CASE("mode 段不可覆盖:用户/项目目录里的 modes 文件不生效") {
    TempDir user("modes");
    TempDir project("modes-proj");
    user.WriteModule("profiles/browser-tester/modes/default.md", "用户想改的模式段。");
    user.WriteModule("modes/default.md", "用户想改的默认模式段。");
    project.WriteModule("profiles/browser-tester/modes/plan.md", "项目想改的 Plan 段。");

    PromptOptions options = GoldenOptions();
    options.prompts_dir = user.Str();
    options.project_prompts_dir = project.Str();
    options.profile = "browser-tester";
    const std::string prompt = AssembleSystemPrompt(options);
    CHECK(Contains(prompt, embedded::kMode_default));   // 宿主内置 default 模式段
    CHECK_FALSE(Contains(prompt, "用户想改的模式段。"));
    CHECK_FALSE(Contains(prompt, "用户想改的默认模式段。"));

    options.plan_mode = true;
    const std::string plan_prompt = AssembleSystemPrompt(options);
    CHECK(Contains(plan_prompt, embedded::kMode_plan));
    CHECK_FALSE(Contains(plan_prompt, "项目想改的 Plan 段。"));
}

TEST_CASE("platform 段按 wire 走五层:Profile 层也能换文案") {
    TempDir user("platform");
    user.WriteModule("profiles/browser-tester/platforms/responses.md", "Profile 的 Responses 方针。");

    PromptOptions options = GoldenOptions();
    options.prompts_dir = user.Str();
    options.profile = "browser-tester";
    options.wire = "responses";
    const std::string prompt = AssembleSystemPrompt(options);
    CHECK(Contains(prompt, "Profile 的 Responses 方针。"));
    CHECK_FALSE(Contains(prompt, embedded::kPlatform_responses));

    // wire 换一个,同名模块不注。
    options.wire = "anthropic";
    const std::string other = AssembleSystemPrompt(options);
    CHECK(Contains(other, embedded::kPlatform_anthropic));
    CHECK_FALSE(Contains(other, "Profile 的 Responses 方针。"));
}

// ---------------------------------------------------------------------------
// 拼装总次序(契约 §6.2):core/persona -> 环境 -> project instructions
// -> features -> platform -> mode -> deferred tool index -> model
// instructions -> Soul。写成断言,不写在注释里。
// ---------------------------------------------------------------------------

TEST_CASE("拼装总次序:九段先后的次序断言(含请求期三层包装)") {
    PromptOptions options = GoldenOptions();
    options.profile = "browser-tester";
    options.project_instructions = "# 项目指令段";
    options.skills_segment = "可用技能:\n- demo: 演示技能";
    options.web = true;  // browser-tester 的 Profile 联网方针正好在这段
    options.wire = "responses";
    const std::string base = AssembleSystemPrompt(options);

    // 请求期三层照契约次序往后叠(与 AgentTool 的用法同一套包装)。
    const std::string deferred = "[延迟工具索引段]";
    const std::string full = WithSoul(
        WithModelInstructions(WithDeferredToolsIndex(base, deferred), "模型专属指令段"), "魂的正文。");

    const auto pos = [&](const std::string& needle) { return full.find(needle); };
    const std::size_t core = pos(EmbeddedProfileText("profiles/browser-tester/core/10-identity.md"));
    const std::size_t env = pos("# 运行环境");
    const std::size_t project = pos("# 项目指令段");
    const std::size_t files = pos(embedded::kFeature_files);
    // web 模块来自内置 Profile(浏览器查验向文案),按文案认位。
    const std::size_t web = pos(EmbeddedProfileText("profiles/browser-tester/features/web.md"));
    const std::size_t platform = pos(embedded::kPlatform_responses);
    const std::size_t mode = pos(embedded::kMode_default);
    const std::size_t index = pos(deferred);
    const std::size_t model = pos("模型专属指令段");
    const std::size_t soul = pos("魂的正文。");

    REQUIRE(core != std::string::npos);
    REQUIRE(env != std::string::npos);
    REQUIRE(project != std::string::npos);
    REQUIRE(files != std::string::npos);
    REQUIRE(web != std::string::npos);
    REQUIRE(platform != std::string::npos);
    REQUIRE(mode != std::string::npos);
    REQUIRE(index != std::string::npos);
    REQUIRE(model != std::string::npos);
    REQUIRE(soul != std::string::npos);

    CHECK(core < env);
    CHECK(env < project);
    CHECK(project < files);
    CHECK(files < web);
    CHECK(web < platform);
    CHECK(platform < mode);
    CHECK(mode < index);
    CHECK(index < model);
    CHECK(model < soul);
}

// ---------------------------------------------------------------------------
// PromptCapabilities(单子 §5.4):从过滤后工具表推导,只给有效能力配说明。
// ---------------------------------------------------------------------------

TEST_CASE("DerivePromptCapabilities:工具名到能力的映射") {
    CHECK(DerivePromptCapabilities({}).web == false);

    PromptCapabilities caps = DerivePromptCapabilities(
        {"read_file", "search", "edit_file", "undo_file_edit", "run_command", "agent", "todo_write",
         "web_search", "web_fetch", "mcp__browser__navigate", "lsp", "skill", "tool_search"});
    CHECK(caps.files);
    CHECK(caps.shell);
    CHECK(caps.delegation);
    CHECK(caps.todo);
    CHECK(caps.web);
    CHECK(caps.mcp);
    CHECK(caps.lsp);

    // 只读白名单(Explore 那五枚):shell/delegation/todo/mcp 都不该有。
    const PromptCapabilities explore =
        DerivePromptCapabilities({"read_file", "search", "web_fetch", "web_search", "lsp"});
    CHECK(explore.files);
    CHECK(explore.web);
    CHECK(explore.lsp);
    CHECK_FALSE(explore.shell);
    CHECK_FALSE(explore.delegation);
    CHECK_FALSE(explore.todo);
    CHECK_FALSE(explore.mcp);
}

TEST_CASE("能力推导下的拼装:裁掉的工具,feature 文案一个字不装") {
    PromptOptions options = GoldenOptions();
    PromptCapabilities caps;  // 只有文件与待办
    caps.files = true;
    caps.todo = true;
    options.capabilities = caps;
    // 父会话的配置开关开着也不算数——自定义 Agent 只认自己的有效工具表。
    options.web = true;
    options.mcp = true;
    options.lsp = true;

    const std::string prompt = AssembleSystemPrompt(options);
    CHECK(Contains(prompt, embedded::kFeature_files));
    CHECK(Contains(prompt, embedded::kFeature_todo));
    CHECK_FALSE(Contains(prompt, embedded::kFeature_shell));
    CHECK_FALSE(Contains(prompt, embedded::kFeature_delegation));
    CHECK_FALSE(Contains(prompt, embedded::kFeature_web));
    CHECK_FALSE(Contains(prompt, embedded::kFeature_mcp));
    CHECK_FALSE(Contains(prompt, embedded::kFeature_lsp));

    // 能力全空:四件套一个不注,只剩环境与模式段(真·无工具 Agent)。
    options.capabilities = PromptCapabilities{};
    const std::string bare = AssembleSystemPrompt(options);
    CHECK_FALSE(Contains(bare, embedded::kFeature_files));
    CHECK(Contains(bare, "# 运行环境"));
    CHECK(Contains(bare, embedded::kMode_default));
}

TEST_CASE("不带能力推导:旧行为一字不差(恒在四件套)") {
    PromptOptions options = GoldenOptions();
    options.capabilities = std::nullopt;
    const std::string prompt = AssembleSystemPrompt(options);
    CHECK(Contains(prompt, embedded::kFeature_files));
    CHECK(Contains(prompt, embedded::kFeature_shell));
    CHECK(Contains(prompt, embedded::kFeature_delegation));
    CHECK(Contains(prompt, embedded::kFeature_todo));
}

// ---------------------------------------------------------------------------
// 来源账本(单子 §5.5):每段从哪层哪文件来。
// ---------------------------------------------------------------------------

TEST_CASE("来源账本:拼进去的每段都有账,来源与次序如实") {
    TempDir user("ledger");
    TempDir project("ledger-proj");
    user.WriteModule("core/20-workflow.md", "用户全局的工作法。");
    user.WriteModule("profiles/browser-tester/features/web.md", "用户层的联网方针。");
    project.WriteModule("profiles/browser-tester/core/10-identity.md", "项目层的身份。");

    PromptOptions options = GoldenOptions();
    options.prompts_dir = user.Str();
    options.project_prompts_dir = project.Str();
    options.profile = "browser-tester";
    options.wire = "responses";
    options.web = true;
    options.project_instructions = "# 项目指令段";
    PromptSourceLedger ledger;
    AssembleSystemPrompt(options, &ledger);

    // 逐段对账。
    const auto* identity = ledger.Find("core/10-identity.md");
    REQUIRE(identity != nullptr);
    CHECK(identity->origin == PromptModuleOrigin::ProjectProfile);
    CHECK(identity->profile == "browser-tester");
    CHECK(Contains(identity->file, "ledger-proj"));

    const auto* workflow = ledger.Find("core/20-workflow.md");
    REQUIRE(workflow != nullptr);
    CHECK(workflow->origin == PromptModuleOrigin::UserDefault);
    CHECK(Contains(workflow->file, "ledger"));

    const auto* style = ledger.Find("core/30-style.md");
    REQUIRE(style != nullptr);
    CHECK(style->origin == PromptModuleOrigin::EmbeddedDefault);
    CHECK(style->file.empty());

    const auto* web = ledger.Find("features/web.md");
    REQUIRE(web != nullptr);
    CHECK(web->origin == PromptModuleOrigin::UserProfile);
    CHECK(web->profile == "browser-tester");

    const auto* platform = ledger.Find("platforms/responses.md");
    REQUIRE(platform != nullptr);
    CHECK(platform->origin == PromptModuleOrigin::EmbeddedDefault);

    const auto* mode = ledger.Find("modes/default.md");
    REQUIRE(mode != nullptr);
    CHECK(mode->origin == PromptModuleOrigin::EmbeddedHostPolicy);

    // 宿主段也记账:环境、项目指令。
    REQUIRE(ledger.Find("(runtime environment)") != nullptr);
    const auto* proj = ledger.Find("(project instructions)");
    REQUIRE(proj != nullptr);
    CHECK(proj->origin == PromptModuleOrigin::ProjectInstructions);

    // 没拼进去的模块不立账(四处开关关着的 web 之外的)。
    CHECK(ledger.Find("platforms/anthropic.md") == nullptr);

    // 账本次序 = 拼装次序:core 各模块打头,mode 殿后。
    CHECK(ledger.entries.front().rel_path == "core/10-identity.md");
    CHECK(ledger.entries.back().rel_path == "modes/default.md");

    // FormatLine 是契约 §6.5 的样张口径。
    CHECK(identity->FormatLine().find("core/10-identity.md <- project profile browser-tester") == 0);
    CHECK(mode->FormatLine() == "modes/default.md <- embedded host policy");
}

TEST_CASE("persona 替换 core 时,账本记一条 persona 替掉整个 core 组") {
    PromptOptions options = GoldenOptions();
    options.persona = "法说了算。";
    PromptSourceLedger ledger;
    AssembleSystemPrompt(options, &ledger);
    const auto* persona = ledger.Find("core/*");
    REQUIRE(persona != nullptr);
    CHECK(persona->origin == PromptModuleOrigin::Persona);
    CHECK(ledger.Find("core/10-identity.md") == nullptr);
    CHECK(ledger.entries.front().rel_path == "core/*");
}

TEST_CASE("BuildPromptProfileLedger:整表记账,含 mode 行,谁压谁一眼可见") {
    TempDir user("whole");
    user.WriteModule("profiles/browser-tester/core/10-identity.md", "用户层的身份。");

    const PromptSourceLedger ledger =
        BuildPromptProfileLedger("browser-tester", user.Str(), std::string());
    CHECK(ledger.entries.size() == std::size(embedded::kAllModules) + 1);  // 全模块 + mode 行
    const auto* identity = ledger.Find("core/10-identity.md");
    REQUIRE(identity != nullptr);
    CHECK(identity->origin == PromptModuleOrigin::UserProfile);
    CHECK(identity->FormatLine() == "core/10-identity.md <- user profile browser-tester (" +
                                       identity->file + ")");
    CHECK(ledger.entries.back().rel_path == "modes/default.md");

    // default 上下文的整表账:模块全走 default 层,mode 行照旧是宿主内置。
    const PromptSourceLedger plain = BuildPromptProfileLedger(std::string(), std::string(), std::string());
    for (const auto& entry : plain.entries) {
        if (entry.rel_path == "modes/default.md") {
            CHECK(entry.origin == PromptModuleOrigin::EmbeddedHostPolicy);
        } else {
            CHECK(entry.origin == PromptModuleOrigin::EmbeddedDefault);
        }
    }
}
