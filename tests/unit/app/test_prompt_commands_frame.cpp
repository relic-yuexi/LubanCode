// TUI 排版批 5b(tui优化todo.todo:/soul /prompt)的输出形状册。
//   - /soul 裸敲:头框(当前魂/锁定状态,句内冒号拆列)-> 魂正文原样跟出
//     (长正文不塞框,批 1 裁量 3)-> 默认值框 -> 可选魂列表框(标题=引导句
//     剥尾冒号)-> 用法框;
//   - /soul off 等分支回执进键值对框;写失败回执上 error 色;
//   - /prompt 裸敲:cmd.prompt.info 首行做框标题,来源/字数/用法拆键值;
//   - plain 主题(--no-color/T3 降级路径)钉零转义字节、无框字形——合同
//     第 3 条;
//   - /soul //prompt 无 --json/--format 分支(源码核实),字节级不变一条
//     天然满足。
//
// 走真 HandleSoulCommand/HandlePromptCommand(私有 HOME 隔离 + TermPort
// 改道捕获),断言只看形状与相对位置。

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>  // std::pair:plain 用例的两路参数

#include "app/commands/prompt_commands.hpp"
#include "cli/terminal_port.hpp"
#include "cli/theme.hpp"
#include "config/config.hpp"
#include "config/prompt_files.hpp"  // DefaultSoulFileContent:fixture 开场默认魂
#include "runtime/session_soul.hpp"

using namespace lubancode;
using lubancode::runtime::SessionSoulSnapshot;

namespace {

class OutputCapture {
public:
    OutputCapture() { cli::TermPort().Redirect(&stream_, nullptr); }
    ~OutputCapture() { cli::TermPort().Reset(); }
    std::string text() const { return stream_.str(); }

private:
    std::ostringstream stream_;
};

std::string StripAnsi(const std::string& text) {
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[') {
            i += 2;
            while (i < text.size() &&
                   !((text[i] >= 'a' && text[i] <= 'z') || (text[i] >= 'A' && text[i] <= 'Z'))) {
                ++i;
            }
            if (i < text.size()) {
                ++i;  // 吃掉终结字母
            }
            continue;
        }
        out += text[i];
        ++i;
    }
    return out;
}

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

constexpr const char* kBoxLightTopLeft = "\xe2\x94\x8c";  // ┌
constexpr const char* kBoxLightVert = "\xe2\x94\x82";     // │

// HOME/USERPROFILE 的作用域替换(与 test_soul_command.cpp 同款)。
class HermeticHome {
public:
    explicit HermeticHome(const std::filesystem::path& dir) {
#ifdef _WIN32
        const char* key = "USERPROFILE";
        const char* original = std::getenv(key);
        original_value_ = original != nullptr ? std::optional<std::string>(original) : std::nullopt;
        _putenv_s(key, dir.string().c_str());
#else
        const char* key = "HOME";
        const char* original = std::getenv(key);
        original_value_ = original != nullptr ? std::optional<std::string>(original) : std::nullopt;
        setenv(key, dir.string().c_str(), /*replace=*/1);
#endif
        key_ = key;
    }
    ~HermeticHome() {
        if (original_value_.has_value()) {
#ifdef _WIN32
            _putenv_s(key_.c_str(), original_value_->c_str());
#else
            setenv(key_.c_str(), original_value_->c_str(), /*replace=*/1);
#endif
        } else {
#ifdef _WIN32
            _putenv_s(key_.c_str(), "");
#else
            unsetenv(key_.c_str());
#endif
        }
    }

private:
    std::string key_;
    std::optional<std::string> original_value_;
};

struct PromptRig {
    std::filesystem::path home;
    std::unique_ptr<HermeticHome> hermetic;
    std::shared_ptr<std::string> configured = std::make_shared<std::string>();
    std::string configured_name = "default";
    SessionSoulSnapshot session;
    cli::Theme theme{cli::BuiltinTheme("dark")};
    cli::Theme plain_theme{cli::BuiltinTheme("plain")};
    static int counter_;

    explicit PromptRig(const std::string& tag) {
        const auto base = std::filesystem::temp_directory_path();
        std::error_code ec;
        for (int i = 0; i < 64; ++i) {
            const auto candidate =
                base / ("lubancode-prompt-frame-" + tag + "-" + std::to_string(++counter_));
            if (std::filesystem::create_directory(candidate, ec)) {
                home = candidate;
                break;
            }
        }
        REQUIRE_FALSE(home.empty());
        hermetic = std::make_unique<HermeticHome>(home);
        *configured = config::DefaultSoulFileContent();
        session.name = "default";
        session.content = *configured;
        session.source = "config:default";
        session.content_hash = runtime::SessionSoulContentHash(session.content);
    }
    ~PromptRig() {
        std::error_code ec;
        std::filesystem::remove_all(home, ec);
    }

    std::string RunSoul(const std::string& args, const cli::Theme& use_theme) {
        OutputCapture capture;
        app::HandleSoulCommand(args, session, configured, configured_name,
                               /*config_file_path=*/std::nullopt, use_theme);
        return capture.text();
    }

    std::string RunPrompt(const std::string& args, const cli::Theme& use_theme) {
        OutputCapture capture;
        app::HandlePromptCommand(args, "law-test", "persona-test", "", use_theme);
        return capture.text();
    }
};
int PromptRig::counter_ = 0;

}  // namespace

TEST_CASE("/soul 裸敲: 头框/正文框外跟出/默认框/可选魂列表/用法框") {
    PromptRig rig("bare");
    rig.session.content = "一腔心事,两处闲愁。";
    const std::string out = rig.RunSoul("", rig.theme);

    REQUIRE(Contains(out, kBoxLightTopLeft));
    const std::string plain = StripAnsi(out);
    // 头框:句内冒号拆列("本会话魂"为 key,魂名为 value)。
    CHECK(Contains(plain, "本会话魂"));
    CHECK(Contains(plain, "default"));
    CHECK(Contains(plain, "锁定状态"));
    CHECK(Contains(plain, "未锁定"));
    // 魂正文不塞框,原样跟出(批 1 裁量 3)。
    CHECK(Contains(plain, "一腔心事,两处闲愁。"));
    // 默认值框与 pending 注。
    CHECK(Contains(plain, "下个新会话默认值"));
    CHECK(Contains(plain, "默认值与当前快照相同"));
    // 可选魂列表:引导句剥尾冒号做框顶,default 行在。
    CHECK(Contains(plain, "可选旧魂(输入名字切换)"));
    CHECK(plain.find("可选旧魂(输入名字切换):") == std::string::npos);
    CHECK(Contains(plain, "(主目录 SOUL.md)"));
    // 用法框。
    CHECK(Contains(plain, "用法"));
    CHECK(Contains(plain, "/soul clear"));
    // key 列走 row_label 色(批 0 合同)。
    CHECK(Contains(out, rig.theme.row_label + "本会话魂"));
}

TEST_CASE("/soul off(未锁定): 回执进键值对框") {
    PromptRig rig("off");
    rig.session.content = "有正文的魂";
    rig.session.content_hash = runtime::SessionSoulContentHash(rig.session.content);
    const std::string out = rig.RunSoul("off", rig.theme);
    REQUIRE(Contains(out, kBoxLightTopLeft));
    CHECK(Contains(StripAnsi(out), "已设置"));  // draft.save_hint 首句
}

TEST_CASE("/prompt usage: 用法句拆列进框") {
    PromptRig rig("usage");
    const std::string out = rig.RunPrompt("bogus", rig.theme);
    REQUIRE(Contains(out, kBoxLightTopLeft));
    const std::string plain = StripAnsi(out);
    CHECK(Contains(plain, "用法"));
    CHECK(Contains(plain, "/prompt reset"));
}

TEST_CASE("/prompt 裸敲: 首行做框标题,来源/字数拆键值") {
    PromptRig rig("prompt");
    const std::string out = rig.RunPrompt("", rig.theme);
    REQUIRE(Contains(out, kBoxLightTopLeft));
    const std::string plain = StripAnsi(out);
    // cmd.prompt.info 首行(剥尾冒号)进框顶,来源/字数/用法各拆键值。
    CHECK(Contains(plain, "当前的法(系统提示词人格段)"));
    CHECK(Contains(plain, "来源"));
    CHECK(Contains(plain, "law-test"));
    CHECK(Contains(plain, "字数"));
    CHECK(Contains(plain, "用法"));
}

TEST_CASE("plain 主题: 两族输出零转义字节、无框字形(T3/--no-color 路径)") {
    PromptRig rig("plain");
    rig.session.content = "plain 形状册的魂正文";
    for (const auto& run : {std::pair<std::string, int>("", 0), std::pair<std::string, int>("off", 1)}) {
        CAPTURE(run.first);
        const std::string soul_out = rig.RunSoul(run.first, rig.plain_theme);
        CHECK(soul_out.find("\x1b") == std::string::npos);
        CHECK(!Contains(soul_out, kBoxLightTopLeft));
        CHECK(!Contains(soul_out, kBoxLightVert));
        CHECK(Contains(soul_out, "本会话魂"));
    }
    const std::string prompt_out = rig.RunPrompt("", rig.plain_theme);
    CHECK(prompt_out.find("\x1b") == std::string::npos);
    CHECK(!Contains(prompt_out, kBoxLightTopLeft));
    CHECK(Contains(prompt_out, "当前的法(系统提示词人格段)"));
}
