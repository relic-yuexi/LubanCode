// prompt_assembler(0.19.x 提示词模块化内置):src/prompts/ 的 .md 模块
// 构建期嵌进 embedded_prompts.hpp,运行时按能力条件拼装。这里测全:
// 嵌入常量非空且含关键短语、core 拼默认人格(跟 DefaultPersona 同源)、
// 恒在段、features 开关矩阵(skills/web/mcp/lsp)、法替换 core、
// 上下文行现填、platform 段按 wire。

#include <doctest/doctest.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "agent/prompt_assembler.hpp"
#include "agent/prompts.hpp"
#include "embedded_prompts.hpp"

using namespace lubancode::agent;

namespace {

// 固定 cwd/日期,断言可复现;开关全默认关。
PromptOptions BaseOptions() {
    PromptOptions options;
    options.cwd = "D:/work";
    options.current_date = "2026-07-18";
    return options;
}

bool Contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

// 运行时化(0.21.x)测试用的临时用户模块目录。
class TempPromptsDir {
public:
    TempPromptsDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("lubancode_prompt_assembler_test_" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path_);
    }
    ~TempPromptsDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    std::string Str() const { return path_.string(); }

    void WriteModule(const std::string& rel_path, const std::string& content) {
        const std::filesystem::path full = path_ / std::filesystem::path(rel_path);
        std::filesystem::create_directories(full.parent_path());
        std::ofstream file(full, std::ios::binary | std::ios::trunc);
        file << content;
    }

private:
    std::filesystem::path path_;
};

}  // namespace

TEST_CASE("嵌入生成:模块常量非空,各含自己的关键短语") {
    // core:至少一个模块,每个都非空,身份模块提到 lubancode。
    REQUIRE(std::size(embedded::kCoreModules) >= 1);
    for (const char* module : embedded::kCoreModules) {
        CHECK(std::strlen(module) > 0);
    }
    CHECK(Contains(embedded::kCoreModules[0], "lubancode"));

    // features:方针段各自点到自己的工具名。
    CHECK(Contains(embedded::kFeature_files, "read_file"));
    CHECK(Contains(embedded::kFeature_files, "edit_file"));
    CHECK(Contains(embedded::kFeature_shell, "run_command"));
    CHECK(Contains(embedded::kFeature_delegation, "agent"));
    CHECK(Contains(embedded::kFeature_todo, "todo_write"));
    CHECK(Contains(embedded::kFeature_skills, "skill"));
    CHECK(Contains(embedded::kFeature_web, "web_search"));
    CHECK(Contains(embedded::kFeature_web, "web_fetch"));
    CHECK(Contains(embedded::kFeature_mcp, "mcp__"));
    CHECK(Contains(embedded::kFeature_lsp, "lsp"));

    // platforms:两个协议段各认各的名号。
    CHECK(Contains(embedded::kPlatform_anthropic, "Anthropic"));
    CHECK(Contains(embedded::kPlatform_responses, "Responses"));
}

TEST_CASE("AssembledDefaultPersona: core 模块按文件名序拼出,DefaultPersona 同源") {
    const std::string persona = AssembledDefaultPersona();
    CHECK(persona.find(embedded::kCoreModules[0]) == 0);  // 第一个模块打头
    for (const char* module : embedded::kCoreModules) {
        CHECK(Contains(persona, module));
    }
    // prompts.hpp 的薄壳跟这里同一个源,法脚手架/reset 还原不再两处维护。
    CHECK(persona == DefaultPersona());
}

TEST_CASE("恒在段:core + 环境 + files/shell/delegation/todo,开关全关也在") {
    const std::string prompt = AssembleSystemPrompt(BaseOptions());
    CHECK(prompt.find(AssembledDefaultPersona()) == 0);
    CHECK(Contains(prompt, embedded::kFeature_files));
    CHECK(Contains(prompt, embedded::kFeature_shell));
    CHECK(Contains(prompt, embedded::kFeature_delegation));
    CHECK(Contains(prompt, embedded::kFeature_todo));
    // 没启用的能力一个字不占。
    CHECK_FALSE(Contains(prompt, embedded::kFeature_skills));
    CHECK_FALSE(Contains(prompt, embedded::kFeature_web));
    CHECK_FALSE(Contains(prompt, embedded::kFeature_mcp));
    CHECK_FALSE(Contains(prompt, embedded::kFeature_lsp));
    CHECK_FALSE(Contains(prompt, embedded::kPlatform_anthropic));
    CHECK_FALSE(Contains(prompt, embedded::kPlatform_responses));
}

TEST_CASE("上下文行:cwd、注入的日期、操作系统都现填在运行环境段里") {
    const std::string prompt = AssembleSystemPrompt(BaseOptions());
    CHECK(Contains(prompt, "# 运行环境"));
    CHECK(Contains(prompt, "- 工作目录: D:/work"));
    CHECK(Contains(prompt, "- 今天日期: 2026-07-18"));
    CHECK(Contains(prompt, "- 操作系统: "));
    // 环境段的硬规矩(该用工具就用)也在。
    CHECK(Contains(prompt, "优先调用工具"));
}

TEST_CASE("上下文行:不注入日期时现取本机日期,YYYY-MM-DD 格式") {
    const std::string seg = BuildEnvironmentSegment("/x");
    const std::size_t pos = seg.find("- 今天日期: ");
    REQUIRE(pos != std::string::npos);
    const std::string date = seg.substr(pos + std::string("- 今天日期: ").size(), 10);
    REQUIRE(date.size() == 10);
    CHECK(date[4] == '-');
    CHECK(date[7] == '-');
    for (const std::size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u}) {
        CHECK(date[i] >= '0');
        CHECK(date[i] <= '9');
    }
}

TEST_CASE("features 开关矩阵:skills/web/mcp/lsp 各自独立开合") {
    SUBCASE("skills:段非空才注模块,清单紧随模块之后") {
        PromptOptions options = BaseOptions();
        options.skills_segment = "可用技能(用 skill 工具按名加载):\n- demo: 演示技能";
        const std::string prompt = AssembleSystemPrompt(options);
        const std::size_t module_pos = prompt.find(embedded::kFeature_skills);
        const std::size_t list_pos = prompt.find("- demo: 演示技能");
        REQUIRE(module_pos != std::string::npos);
        REQUIRE(list_pos != std::string::npos);
        CHECK(list_pos > module_pos);
    }
    SUBCASE("web 开") {
        PromptOptions options = BaseOptions();
        options.web = true;
        const std::string prompt = AssembleSystemPrompt(options);
        CHECK(Contains(prompt, embedded::kFeature_web));
        CHECK_FALSE(Contains(prompt, embedded::kFeature_mcp));
        CHECK_FALSE(Contains(prompt, embedded::kFeature_lsp));
    }
    SUBCASE("mcp 开") {
        PromptOptions options = BaseOptions();
        options.mcp = true;
        const std::string prompt = AssembleSystemPrompt(options);
        CHECK(Contains(prompt, embedded::kFeature_mcp));
        CHECK_FALSE(Contains(prompt, embedded::kFeature_web));
        CHECK_FALSE(Contains(prompt, embedded::kFeature_lsp));
    }
    SUBCASE("lsp 开") {
        PromptOptions options = BaseOptions();
        options.lsp = true;
        const std::string prompt = AssembleSystemPrompt(options);
        CHECK(Contains(prompt, embedded::kFeature_lsp));
        CHECK_FALSE(Contains(prompt, embedded::kFeature_web));
        CHECK_FALSE(Contains(prompt, embedded::kFeature_mcp));
    }
    SUBCASE("全开:四段都在,次序 skills < web < mcp < lsp") {
        PromptOptions options = BaseOptions();
        options.skills_segment = "可用技能(用 skill 工具按名加载):\n- demo: 演示技能";
        options.web = true;
        options.mcp = true;
        options.lsp = true;
        const std::string prompt = AssembleSystemPrompt(options);
        const std::size_t skills_pos = prompt.find(embedded::kFeature_skills);
        const std::size_t web_pos = prompt.find(embedded::kFeature_web);
        const std::size_t mcp_pos = prompt.find(embedded::kFeature_mcp);
        const std::size_t lsp_pos = prompt.find(embedded::kFeature_lsp);
        REQUIRE(skills_pos != std::string::npos);
        REQUIRE(web_pos != std::string::npos);
        REQUIRE(mcp_pos != std::string::npos);
        REQUIRE(lsp_pos != std::string::npos);
        CHECK(skills_pos < web_pos);
        CHECK(web_pos < mcp_pos);
        CHECK(mcp_pos < lsp_pos);
    }
}

TEST_CASE("法替换 core:人格非空时 core 让位,环境/features 段照拼") {
    PromptOptions options = BaseOptions();
    options.persona = "你只用文言文答话。";
    options.web = true;
    const std::string prompt = AssembleSystemPrompt(options);
    CHECK(prompt.find("你只用文言文答话。") == 0);  // 法打头
    CHECK_FALSE(Contains(prompt, AssembledDefaultPersona().c_str()));
    // 环境段、恒在 features、条件 features 一样不少。
    CHECK(Contains(prompt, "- 工作目录: D:/work"));
    CHECK(Contains(prompt, embedded::kFeature_files));
    CHECK(Contains(prompt, embedded::kFeature_web));
}

TEST_CASE("platform 段按 wire:anthropic/responses 各注各的,认不出不注") {
    PromptOptions options = BaseOptions();

    options.wire = "anthropic";
    std::string prompt = AssembleSystemPrompt(options);
    CHECK(Contains(prompt, embedded::kPlatform_anthropic));
    CHECK_FALSE(Contains(prompt, embedded::kPlatform_responses));

    options.wire = "responses";
    prompt = AssembleSystemPrompt(options);
    CHECK(Contains(prompt, embedded::kPlatform_responses));
    CHECK_FALSE(Contains(prompt, embedded::kPlatform_anthropic));

    options.wire = "别的什么";
    prompt = AssembleSystemPrompt(options);
    CHECK_FALSE(Contains(prompt, embedded::kPlatform_anthropic));
    CHECK_FALSE(Contains(prompt, embedded::kPlatform_responses));
}

// ---------------------------------------------------------------------------
// 运行时化(0.21.x):prompts_dir 非空时逐模块"用户文件优先、嵌入回退"。
// ---------------------------------------------------------------------------

TEST_CASE("自我认知:身份模块带'先加载 lubancode-config'的硬规矩") {
    CHECK(Contains(embedded::kCoreModules[0], "lubancode-config"));
    CHECK(Contains(embedded::kCoreModules[0], "skill"));
}

TEST_CASE("kAllModules 总表:与分组常量同源,core 在前、相对路径规范") {
    REQUIRE(std::size(embedded::kAllModules) >= 13);  // 3 core + 8 features + 2 platforms
    CHECK(std::string(embedded::kAllModules[0].rel_path) == "core/10-identity.md");
    CHECK(embedded::kAllModules[0].content == embedded::kCoreModules[0]);
    for (const auto& module : embedded::kAllModules) {
        CHECK(std::strlen(module.content) > 0);
        const std::string rel = module.rel_path;
        CHECK((rel.rfind("core/", 0) == 0 || rel.rfind("features/", 0) == 0 || rel.rfind("platforms/", 0) == 0));
        CHECK(rel.size() > 3);
        CHECK(rel.substr(rel.size() - 3) == ".md");
    }
}

TEST_CASE("PromptModuleSeeds: 清单覆盖全部模块,内容就是嵌入正文") {
    const auto seeds = PromptModuleSeeds();
    REQUIRE(seeds.size() == std::size(embedded::kAllModules));
    bool found_identity = false;
    for (const auto& [rel, content] : seeds) {
        if (rel == "core/10-identity.md") {
            found_identity = true;
            CHECK(content == embedded::kCoreModules[0]);
        }
    }
    CHECK(found_identity);
}

TEST_CASE("运行时覆盖:用户 features 模块文件压过嵌入版,别的模块不受牵连") {
    TempPromptsDir dir;
    dir.WriteModule("features/files.md", "用户自定义的文件方针:改文件前唱一段昆曲。");

    PromptOptions options = BaseOptions();
    options.prompts_dir = dir.Str();
    const std::string prompt = AssembleSystemPrompt(options);

    CHECK(Contains(prompt, "改文件前唱一段昆曲"));
    CHECK_FALSE(Contains(prompt, embedded::kFeature_files));
    // 没覆盖的模块照用嵌入版。
    CHECK(Contains(prompt, embedded::kFeature_shell));
    CHECK(prompt.find(AssembledDefaultPersona()) == 0);  // core 没覆盖,仍是嵌入默认
}

TEST_CASE("运行时覆盖:用户 core 模块参与拼默认人格,persona 非空时让位") {
    TempPromptsDir dir;
    dir.WriteModule("core/10-identity.md", "# 身份\n\n你是测试专用的木鸢,末尾带一句【木鸢】。");

    PromptOptions options = BaseOptions();
    options.prompts_dir = dir.Str();
    std::string prompt = AssembleSystemPrompt(options);
    CHECK(prompt.find("# 身份\n\n你是测试专用的木鸢") == 0);  // 覆盖的身份模块打头
    CHECK(Contains(prompt, embedded::kCoreModules[1]));       // 其余 core 模块照旧

    // AssembledCorePersona 同一套账;AssembledDefaultPersona 恒用嵌入版。
    CHECK(AssembledCorePersona(dir.Str()).find("木鸢") != std::string::npos);
    CHECK(AssembledDefaultPersona().find("木鸢") == std::string::npos);

    // 法(persona)非空时整段替换,用户 core 模块让位。
    options.persona = "法说了算。";
    prompt = AssembleSystemPrompt(options);
    CHECK(prompt.find("法说了算。") == 0);
    CHECK_FALSE(Contains(prompt, "木鸢"));
}

TEST_CASE("运行时覆盖:用户文件是空白/CRLF 归一后为空 → 回退嵌入版") {
    TempPromptsDir dir;
    dir.WriteModule("features/shell.md", "  \r\n\t \r\n");

    PromptOptions options = BaseOptions();
    options.prompts_dir = dir.Str();
    const std::string prompt = AssembleSystemPrompt(options);
    CHECK(Contains(prompt, embedded::kFeature_shell));  // 空白文件不算数
}

TEST_CASE("运行时覆盖:CRLF 行尾归一,和嵌入版一致就逐字节相同") {
    TempPromptsDir dir;
    // 把嵌入版原文换成 CRLF 行尾写盘——归一后应与嵌入版逐字节一致。
    std::string crlf;
    for (const char c : std::string(embedded::kFeature_todo)) {
        if (c == '\n') {
            crlf += "\r\n";
        } else {
            crlf += c;
        }
    }
    dir.WriteModule("features/todo.md", crlf + "\r\n");

    PromptOptions options = BaseOptions();
    options.prompts_dir = dir.Str();
    const std::string prompt = AssembleSystemPrompt(options);
    CHECK(Contains(prompt, embedded::kFeature_todo));
    CHECK_FALSE(Contains(prompt, "\r"));

    // 来源统计:CRLF 文件也算"用户文件"(非空即算,内容一致与否不问)。
    const auto sources = PromptModuleSources(dir.Str());
    for (const auto& source : sources) {
        if (source.rel_path == "features/todo.md") {
            CHECK(source.from_user_file);
            CHECK_FALSE(source.differs_from_embedded);  // 归一后与嵌入版一致 = 没改过
        }
    }
}

TEST_CASE("PromptModuleSources: 用户文件/内置来源统计") {
    TempPromptsDir dir;
    dir.WriteModule("core/30-style.md", "答话一律带笑。");
    dir.WriteModule("platforms/anthropic.md", "平台段自定义。");
    dir.WriteModule("features/web.md", "   ");  // 空白 = 不算用户文件

    const auto sources = PromptModuleSources(dir.Str());
    REQUIRE(sources.size() == std::size(embedded::kAllModules));
    std::size_t user_count = 0;
    for (const auto& source : sources) {
        if (source.from_user_file) {
            ++user_count;
            CHECK((source.rel_path == "core/30-style.md" || source.rel_path == "platforms/anthropic.md"));
            CHECK(source.differs_from_embedded);  // 内容偏离嵌入版 = 已改
        }
    }
    CHECK(user_count == 2);

    // prompts_dir 为空(找不到主目录)= 全内置。
    for (const auto& source : PromptModuleSources(std::string())) {
        CHECK_FALSE(source.from_user_file);
    }
}

TEST_CASE("项目 AGENTS 指令作为独立段注入") {
    PromptOptions options = BaseOptions();
    options.project_instructions = "# Project Instructions\n\n- run the focused tests";
    const std::string prompt = AssembleSystemPrompt(options);
    CHECK(prompt.find(options.project_instructions) != std::string::npos);
    CHECK(prompt.find(options.project_instructions) > prompt.find("# 运行环境"));
    CHECK(prompt.find(options.project_instructions) < prompt.find("# 文件读写"));
}

// P1-2 逐 source 账:来源清单非空时,账本每份文档一行(哪层哪个文件);
// 没递清单(旧装配/单测)照旧一条总项——零退化。
TEST_CASE("项目指令逐 source 记账:清单非空每份一行,空则照旧一条") {
    PromptOptions options = BaseOptions();
    options.project_instructions = "# Project Instructions\n\n- run the focused tests";

    // 旧口径:只递拼接串,账本压成一条总项,file 为空。
    {
        PromptSourceLedger ledger;
        AssembleSystemPrompt(options, &ledger);
        std::size_t count = 0;
        for (const PromptSourceLedgerEntry& entry : ledger.entries) {
            if (entry.origin == PromptModuleOrigin::ProjectInstructions) {
                ++count;
                CHECK(entry.file.empty());
            }
        }
        CHECK(count == 1);
    }

    // 新口径:链上两份文档,账本两行,各自带文件来源,FormatLine 亮出来。
    {
        options.project_instruction_sources = {"D:/repo/AGENTS.md", "D:/repo/src/AGENTS.md"};
        PromptSourceLedger ledger;
        AssembleSystemPrompt(options, &ledger);
        std::size_t count = 0;
        std::size_t with_file = 0;
        bool saw_root_line = false;
        for (const PromptSourceLedgerEntry& entry : ledger.entries) {
            if (entry.origin != PromptModuleOrigin::ProjectInstructions) {
                continue;
            }
            ++count;
            if (!entry.file.empty()) {
                ++with_file;
            }
            if (entry.file == "D:/repo/AGENTS.md") {
                CHECK(Contains(entry.FormatLine(), "D:/repo/AGENTS.md"));
                saw_root_line = true;
            }
        }
        CHECK(count == 2);
        CHECK(with_file == 2);
        CHECK(saw_root_line);
    }
}
