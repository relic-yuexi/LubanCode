// i18n(界面多语言):tr 回退链、语言包加载/覆盖、坏 JSON 跳过、系统语言
// 映射、/language 解析、向导语言步。
//
// 注意:语言是进程级全局状态,每个会改语言/装语言包的用例用 LangGuard
// 兜底还原(current 回 zh-CN、语言包清空),免得串到别的测试文件头上——
// 全部旧测试都默认 zh-CN 现状文案。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "cli/i18n.hpp"
#include "cli/setup_wizard.hpp"
#include "cli/slash_commands.hpp"
#include "cli/transcript.hpp"
#include "config/config.hpp"

using namespace lubancode;

namespace {

// 还原语言状态:析构时切回 zh-CN、清空用户语言包(扫一个不存在的目录,
// LoadLanguagePacksFromDir 整体替换成空)。
struct LangGuard {
    ~LangGuard() {
        cli::LoadLanguagePacksFromDir("Z:/definitely/not/a/dir/lubancode-i18n-test");
        cli::SetLanguage("zh-CN");
    }
};

// 建一个临时语言包目录,写入若干文件,返回目录路径(UTF-8)。
std::filesystem::path MakeTempLangDir(const std::string& tag) {
    const auto dir = std::filesystem::temp_directory_path() / ("lubancode_i18n_" + tag);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

void WriteFileUtf8(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

}  // namespace

TEST_CASE("tr: 默认语言是 zh-CN,取到的就是中文原文") {
    LangGuard guard;
    cli::SetLanguage("zh-CN");
    CHECK(cli::tr("cmd.clear.done") == "已清空对话历史。");
    CHECK(cli::tr("banner.hint") == "输入问题回车发送,exit 退出,/help 看命令");
}

TEST_CASE("tr: en 有键取英文,en 缺键回退 zh-CN,谁都没有回退 key 本身") {
    LangGuard guard;
    cli::SetLanguage("en");
    // P0 键:en 表有。
    CHECK(cli::tr("slash.desc.clear") == "clear the conversation history");
    CHECK(cli::tr("cmd.clear.done") == "已清空对话历史。");  // P1 缺英文,回退 zh-CN
    CHECK(cli::tr("error.prefix") == "[error] ");
    // 完全不存在的键:回退 key 本身。
    CHECK(cli::tr("no.such.key.at.all") == "no.such.key.at.all");
}

TEST_CASE("TrFormat: {0}/{1} 按序替换,JSON 花括号不受牵连,缺实参原样保留") {
    LangGuard guard;
    cli::SetLanguage("zh-CN");
    CHECK(cli::trf("cmd.model.switched", "gpt-x") == "已切换到模型: gpt-x(本会话生效)");
    CHECK(cli::trf("transcript.added_removed", 3, 5) == "新增 3 行,删除 5 行");
    // help.config 里有 {"mcpServers": ...} 这类 JSON 示例,TrFormat 只认
    // {数字},别的花括号原样保留。
    const std::string help = cli::trf("help.config", 1, "dark", 2);
    CHECK(help.find("{\"command\"") != std::string::npos);
    CHECK(help.find("max_context_chars=1") != std::string::npos);
    // 缺实参:占位符原样保留,好排查。
    CHECK(cli::trf("transcript.added_removed", 3) == "新增 3 行,删除 {1} 行");
}

TEST_CASE("自动会话标题提示:标题先填入,再附 cheap 路由,不漏占位符") {
    LangGuard guard;
    cli::SetLanguage("zh-CN");
    const std::string notice =
        cli::trf("router.task_flash", cli::trf("cmd.title.set", "New Session"), "cheap:glm-5.2");
    CHECK(notice == "标题已设为: New Session · cheap:glm-5.2");
    CHECK(notice.find("{0}") == std::string::npos);
    CHECK(notice.find("首条消息落盘后") == std::string::npos);
}

TEST_CASE("/model 编号提示写明 Esc 可取消") {
    LangGuard guard;
    cli::SetLanguage("zh-CN");
    CHECK(cli::trf("cmd.model.choose", 16) == "选择模型编号 [16]（Esc 取消）: ");
    CHECK(cli::tr("cmd.model.current") == "  ← 当前");
    CHECK(cli::tr("cmd.model.cancelled") == "已取消模型切换。");
}

TEST_CASE("语言包: ja.json 生效,盖到的键出日文,其余回退 zh-CN,列表出现 ja") {
    LangGuard guard;
    const auto dir = MakeTempLangDir("ja");
    WriteFileUtf8(dir / "ja.json",
                  R"json({"language.name": "日本語 (ja)", "banner.hint": "質問を入力して Enter で送信"})json");
    const auto warnings = cli::LoadLanguagePacksFromDir(dir.string());
    CHECK(warnings.empty());

    const auto langs = cli::AvailableLanguages();
    bool has_ja = false;
    for (const auto& code : langs) {
        if (code == "ja") {
            has_ja = true;
        }
    }
    CHECK(has_ja);
    CHECK(cli::HasLanguage("ja"));
    CHECK(cli::LanguageDisplayName("ja") == "日本語 (ja)");

    cli::SetLanguage("ja");
    CHECK(cli::tr("banner.hint") == "質問を入力して Enter で送信");
    // ja 包没有的键:回退 zh-CN。
    CHECK(cli::tr("cmd.clear.done") == "已清空对话历史。");
    std::filesystem::remove_all(dir);
}

TEST_CASE("语言包: zh-CN.json 覆盖内置同键(用户改措辞的口子)") {
    LangGuard guard;
    const auto dir = MakeTempLangDir("override");
    WriteFileUtf8(dir / "zh-CN.json", R"({"cmd.clear.done": "history 已清"})");
    const auto warnings = cli::LoadLanguagePacksFromDir(dir.string());
    CHECK(warnings.empty());

    cli::SetLanguage("zh-CN");
    CHECK(cli::tr("cmd.clear.done") == "history 已清");
    // 没盖的键照旧内置。
    CHECK(cli::tr("banner.hint") == "输入问题回车发送,exit 退出,/help 看命令");
    // 可选语言列表不因 zh-CN.json 多出一项(内置已有,去重)。
    std::size_t zh_count = 0;
    for (const auto& code : cli::AvailableLanguages()) {
        if (code == "zh-CN") {
            ++zh_count;
        }
    }
    CHECK(zh_count == 1);
    std::filesystem::remove_all(dir);
}

TEST_CASE("语言包: 坏 JSON / 顶层不是 object / 值不是字符串,警告 + 整文件跳过") {
    LangGuard guard;
    const auto dir = MakeTempLangDir("bad");
    WriteFileUtf8(dir / "fr.json", "{ this is not json");
    WriteFileUtf8(dir / "de.json", R"(["array", "not", "object"])");
    WriteFileUtf8(dir / "ko.json", R"({"a.key": 42})");
    WriteFileUtf8(dir / "ja.json", R"({"banner.hint": "OK"})");  // 好的这份照常装上
    const auto warnings = cli::LoadLanguagePacksFromDir(dir.string());
    CHECK(warnings.size() == 3);
    CHECK_FALSE(cli::HasLanguage("fr"));
    CHECK_FALSE(cli::HasLanguage("de"));
    CHECK_FALSE(cli::HasLanguage("ko"));
    CHECK(cli::HasLanguage("ja"));
    std::filesystem::remove_all(dir);
}

TEST_CASE("语言包: 目录里混着子目录/悬空链接,跳过不崩,好包照装") {
    // 修复钉子:目录扫描一度用 throwing 版 is_regular_file() 和 range-for
    // 的 operator++,悬空符号链接/权限变动会抛 filesystem_error 直接掀翻
    // 启动。现在全走 error_code 重载——脏条目跳过,好包照装,永不抛。
    // "扫描中途权限变动/ACCESS_DENIED"这类真会让 error_code 置位的场景
    // 没法在单测里稳定摆出来(要跨进程改 ACL),那半边靠走查:增量与状态
    // 全用 ec 重载,失败只落警告,见 i18n.cpp LoadLanguagePacksFromDir 注释。
    LangGuard guard;
    const auto dir = MakeTempLangDir("dirty");
    WriteFileUtf8(dir / "ja.json", R"({"banner.hint": "OK"})");
    std::filesystem::create_directories(dir / "sub.json");  // 顶着 .json 名的子目录
    std::error_code link_ec;
    // 悬空符号链接:Windows 没开发者模式/管理员权限时建不出来——建不成就
    // 只靠子目录那半边(is_regular_file 的 ec 重载对两者同一条路)。
    std::filesystem::create_symlink(dir / "no_such_target.json", dir / "dangling.json", link_ec);

    const auto warnings = cli::LoadLanguagePacksFromDir(dir.string());
    CHECK(warnings.empty());  // 脏条目静默跳过,跟"非 .json 不理会"同一待遇
    CHECK(cli::HasLanguage("ja"));
    CHECK_FALSE(cli::HasLanguage("sub"));
    CHECK_FALSE(cli::HasLanguage("dangling"));
    std::filesystem::remove_all(dir);
}

TEST_CASE("MapLocaleToLanguage: zh 前缀→zh-CN,en 前缀→en,认不出→zh-CN") {
    CHECK(cli::MapLocaleToLanguage("zh-CN") == "zh-CN");
    CHECK(cli::MapLocaleToLanguage("zh-TW") == "zh-CN");
    CHECK(cli::MapLocaleToLanguage("zh") == "zh-CN");
    CHECK(cli::MapLocaleToLanguage("en-US") == "en");
    CHECK(cli::MapLocaleToLanguage("en_GB.UTF-8") == "en");
    CHECK(cli::MapLocaleToLanguage("en") == "en");
    CHECK(cli::MapLocaleToLanguage("ja-JP") == "zh-CN");
    CHECK(cli::MapLocaleToLanguage("fr") == "zh-CN");
    CHECK(cli::MapLocaleToLanguage("") == "zh-CN");
    // "eo"(世界语)不是 en:前两个字母相同也不能误判,得看边界。
    CHECK(cli::MapLocaleToLanguage("eo") == "zh-CN");
    CHECK(cli::MapLocaleToLanguage("english") == "zh-CN");  // en 后跟字母,不算 en 前缀
}

TEST_CASE("ParseSlashCommand: /language 与 /lang 别名,参数是语言码") {
    CHECK(cli::ParseSlashCommand("/language").command == cli::SlashCommand::Language);
    CHECK(cli::ParseSlashCommand("/lang").command == cli::SlashCommand::Language);
    const auto parsed = cli::ParseSlashCommand("/language en");
    CHECK(parsed.command == cli::SlashCommand::Language);
    CHECK(parsed.args == "en");
    CHECK(cli::ParseSlashCommand("/LANGUAGE zh-CN").command == cli::SlashCommand::Language);
}

TEST_CASE("AllSlashCommands: /language 在表里,语言切换后描述跟着换") {
    LangGuard guard;
    cli::SetLanguage("zh-CN");
    bool found_zh = false;
    for (const auto& c : cli::AllSlashCommands()) {
        if (c.name == "/language") {
            found_zh = true;
            CHECK(c.description == cli::tr("slash.desc.language"));
        }
    }
    CHECK(found_zh);
    cli::SetLanguage("en");
    for (const auto& c : cli::AllSlashCommands()) {
        if (c.name == "/clear") {
            CHECK(c.description == "clear the conversation history");
        }
    }
}

TEST_CASE("transcript 摘要词: en 下出英文(彩色主题摘要进表)") {
    LangGuard guard;
    cli::SetLanguage("en");
    CHECK(cli::ReadFileDoneSummary("a\nb\n") == "Read 2 lines");
    CHECK(cli::WriteDiffSummary(3, 5) == "Added 3 lines, removed 5 lines");
    CHECK(cli::SearchDoneSummary("a.cpp:1:x\n") == "1 matches");
    CHECK(cli::AgentDoneSummary(2, 4) == "Subagent 2 steps · 4 tool calls");
    // plain 主题状态词不进表,保持英文常量,跟语言无关。
    CHECK(cli::TranscriptStatusWord(cli::TranscriptStatus::Running) == "[RUNNING]");
}

TEST_CASE("MergeConfig: language 四级合并——env 压配置文件,配置文件压默认空") {
    config::LubancodeEnvValues env;
    config::FileConfig file;
    file.language = "en";
    file.source_path = "/tmp/.lubancode/config.json";

    // 只有配置文件。
    auto result = config::MergeConfig(env, file, config::GenericEnvValues{});
    REQUIRE(result.has_value());
    CHECK(result->config.language == "en");
    CHECK(result->sources.language == config::Source::ProjectConfigFile);

    // env 压过配置文件。
    env.language = "ja";
    result = config::MergeConfig(env, file, config::GenericEnvValues{});
    REQUIRE(result.has_value());
    CHECK(result->config.language == "ja");
    CHECK(result->sources.language == config::Source::LubancodeEnv);

    // 四级全空:留空(= 跟系统),来源 Default。
    result = config::MergeConfig(config::LubancodeEnvValues{}, std::nullopt, config::GenericEnvValues{});
    REQUIRE(result.has_value());
    CHECK(result->config.language.empty());
    CHECK(result->sources.language == config::Source::Default);
}

TEST_CASE("ParseFileConfigJson: language 字段认字符串,别的类型报错") {
    const auto ok = config::ParseFileConfigJson(R"({"language": "en"})", "/tmp/c.json");
    REQUIRE(ok.has_value());
    REQUIRE(ok->language.has_value());
    CHECK(*ok->language == "en");

    const auto bad = config::ParseFileConfigJson(R"({"language": 42})", "/tmp/c.json");
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().find("language") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 向导语言步:第一问选语言,选完向导后续文案即用所选语言,选择写进
// outcome.config.language。
// ---------------------------------------------------------------------------

namespace {

struct ScriptedWizardIO {
    std::vector<std::string> inputs;
    std::size_t next = 0;
    std::vector<std::string> printed;

    cli::WizardIO Build() {
        cli::WizardIO io;
        io.print = [this](const std::string& line) { printed.push_back(line); };
        io.read_line = [this]() -> std::optional<std::string> {
            if (next >= inputs.size()) {
                return std::nullopt;
            }
            return inputs[next++];
        };
        io.fetch_models = [](config::Wire, const std::string&,
                             const std::string&) -> std::expected<std::vector<api::ModelInfo>, api::Error> {
            return std::vector<api::ModelInfo>{};
        };
        io.home_config_display_path = "/fake/home/.lubancode/config.json";
        return io;
    }

    bool AnyPrintedContains(const std::string& needle) const {
        for (const auto& line : printed) {
            if (line.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
};

}  // namespace

TEST_CASE("向导语言步: 选 en 后,向导后续文案是英文,language 进配置") {
    LangGuard guard;
    cli::SetLanguage("zh-CN");
    ScriptedWizardIO scripted;
    scripted.inputs = {
        "2",                                  // 语言:2) English (en)
        "1",                                  // wire = anthropic
        "https://api.example.com/anthropic",  // base_url
        "the-key",                            // api_key
        "SomeModel",                          // model 手输
        "n",                                  // 不保存
    };
    auto io = scripted.Build();
    const auto outcome = cli::RunSetupWizard(io);
    REQUIRE(outcome.has_value());
    CHECK(outcome->config.language == "en");
    CHECK(cli::CurrentLanguage() == "en");
    // 语言步之后的 wire 提问已是英文。
    CHECK(scripted.AnyPrintedContains("Wire protocol:"));
    CHECK(scripted.AnyPrintedContains("  language = en"));
}

TEST_CASE("向导语言步: 直接回车按默认(当前语言),后续文案维持中文") {
    LangGuard guard;
    cli::SetLanguage("zh-CN");
    ScriptedWizardIO scripted;
    scripted.inputs = {
        "",                                   // 语言:回车 = 默认(zh-CN)
        "1",                                  // wire
        "https://api.example.com/anthropic",  // base_url
        "the-key",                            // api_key
        "SomeModel",                          // model
        "n",                                  // 不保存
    };
    auto io = scripted.Build();
    const auto outcome = cli::RunSetupWizard(io);
    REQUIRE(outcome.has_value());
    CHECK(outcome->config.language == "zh-CN");
    CHECK(scripted.AnyPrintedContains("接口格式:"));
}

TEST_CASE("图片界面文案中英都有") {
    LangGuard guard;
    cli::SetLanguage("zh-CN");
    CHECK(cli::trf("image.attached", "err.png", 12, 34) == "[图片] 已附 err.png (12x34)");
    CHECK(cli::tr("error.image.too_large").find("5MB") != std::string::npos);

    cli::SetLanguage("en");
    CHECK(cli::trf("image.attached", "err.png", 12, 34) == "[image] attached err.png (12x34)");
    CHECK(cli::tr("error.image.too_large").find("5MB") != std::string::npos);
}

TEST_CASE("/model roles 在补全说明与帮助里中英都有入口") {
    LangGuard guard;
    for (const std::string language : {"zh-CN", "en"}) {
        cli::SetLanguage(language);
        CHECK(cli::tr("slash.desc.model").find("/model roles") != std::string::npos);
        CHECK(cli::tr("help.slash").find("/model roles") != std::string::npos);
        CHECK(cli::tr("slash_help.body").find("/model roles") != std::string::npos);
        const std::string header = cli::tr("cmd.model.roles_header");
        const bool names_subagents = header.find("子代理") != std::string::npos ||
                                     header.find("subagent") != std::string::npos ||
                                     header.find("sub-agents") != std::string::npos;
        CHECK(names_subagents);
    }
}
