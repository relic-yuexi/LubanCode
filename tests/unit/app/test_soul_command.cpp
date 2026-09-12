// Soul 会话冻结单 P0(/soul 命令分支,§5.2 表):锁定前保存默认并更新草稿;
// 锁定后只改默认、快照不动;内容规范化后相同不记虚假 revision;off 不删
// 正文、clear 才清空;写入失败整体回滚不留内存/磁盘两份账;多会话并存
// 不串值。环境隔离:HOME/USERPROFILE 指到本册私有临时目录(命令层读
// HomeLubancodeDir 走这两个变量),案末恢复原值。

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>

#include "app/commands/prompt_commands.hpp"
#include "config/config.hpp"  // HomeLubancodeDir:命令层读魂文件的同一只口
#include "config/prompt_files.hpp"
#include "runtime/session_soul.hpp"

using namespace lubancode;
using lubancode::runtime::SessionSoulSnapshot;

namespace {

// HOME/USERPROFILE 的作用域替换:构造时换到私有目录,析构恢复原值。
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

// 一案一套:私有 HOME + 两本账(configured 默认的内存映像 / 会话快照)。
struct SoulCommandFixture {
    std::filesystem::path home;
    std::unique_ptr<HermeticHome> hermetic;
    std::shared_ptr<std::string> configured = std::make_shared<std::string>();
    std::string configured_name = "default";
    SessionSoulSnapshot session;

    explicit SoulCommandFixture(const std::string& tag) {
        const auto base = std::filesystem::temp_directory_path();
        std::error_code ec;
        for (int i = 0; i < 64; ++i) {
            const auto candidate = base / ("lubancode-soul-cmd-" + tag + "-" + std::to_string(++counter_));
            if (std::filesystem::create_directory(candidate, ec)) {
                home = candidate;
                break;
            }
        }
        REQUIRE_FALSE(home.empty());
        hermetic = std::make_unique<HermeticHome>(home);
        // 开场:configured 与会话草稿都是默认空魂(与 SessionStack 起手同拍)。
        *configured = config::DefaultSoulFileContent();
        session.name = "default";
        session.content = *configured;
        session.source = "config:default";
        session.content_hash = runtime::SessionSoulContentHash(session.content);
    }
    ~SoulCommandFixture() {
        std::error_code ec;
        std::filesystem::remove_all(home, ec);
    }

    void Run(const std::string& args) {
        app::HandleSoulCommand(args, session, configured, configured_name, /*config_file_path=*/std::nullopt);
    }

    static int counter_;
};
int SoulCommandFixture::counter_ = 0;

}  // namespace

TEST_CASE("/soul <内容>(未锁定):保存默认并更新草稿,revision 递增") {
    SoulCommandFixture fx("write");
    fx.Run("新魂正文:短句,多动词");
    CHECK(fx.configured_name == "default");
    CHECK(*fx.configured == "新魂正文:短句,多动词");
    CHECK(fx.session.name == "default");
    CHECK(fx.session.content == "新魂正文:短句,多动词");
    CHECK(fx.session.revision == 1);
    CHECK_FALSE(fx.session.locked);
}

TEST_CASE("内容规范化后相同:不记虚假 revision") {
    SoulCommandFixture fx("same");
    fx.Run("同一份魂");
    CHECK(fx.session.revision == 1);
    // 注释与空白差异在规范化后相同:不算变更。
    fx.Run("<!-- 说明 -->\n  同一份魂  \n");
    CHECK(fx.session.revision == 1);
    CHECK(fx.session.content == "同一份魂");
}

TEST_CASE("/soul off(未锁定):默认选择 off,草稿关闭;正文文件不删") {
    SoulCommandFixture fx("off");
    fx.Run("有正文的魂");
    fx.Run("off");
    CHECK(fx.configured_name == "off");
    CHECK(fx.session.name == "off");
    CHECK(fx.session.content.empty());
    // off 不删正文:SOUL.md 还在,内容原样。
    const auto soul_file = config::ReadSoulFile(config::HomeLubancodeDir().value());
    REQUIRE(soul_file.has_value());
    CHECK(*soul_file == "有正文的魂");
}

TEST_CASE("/soul clear(未锁定):清空默认正文,配置归位 default") {
    SoulCommandFixture fx("clear");
    fx.Run("会被清掉的魂");
    fx.Run("clear");
    CHECK(fx.configured_name == "default");
    CHECK(*fx.configured == config::DefaultSoulFileContent());
    CHECK(fx.session.name == "default");
    CHECK(fx.session.content == config::DefaultSoulFileContent());
}

TEST_CASE("/soul default(未锁定):解析默认文件并选它") {
    SoulCommandFixture fx("dflt");
    REQUIRE(config::WriteSoulFile(config::HomeLubancodeDir().value(), "SOUL.md 里的魂").has_value());
    fx.Run("default");
    CHECK(fx.configured_name == "default");
    CHECK(*fx.configured == "SOUL.md 里的魂");
    CHECK(fx.session.content == "SOUL.md 里的魂");
}

TEST_CASE("/soul <名字>(未锁定):命中 souls/<名字>.md 时选具名魂") {
    SoulCommandFixture fx("named");
    const auto luban_dir = std::filesystem::path(config::HomeLubancodeDir().value());
    std::filesystem::create_directories(luban_dir / "souls");
    {
        std::error_code ec;
        std::ofstream out(luban_dir / "souls" / "wenge.md", std::ios::binary | std::ios::trunc);
        out << "文风魂:明清白话";
    }
    fx.Run("wenge");
    CHECK(fx.configured_name == "wenge");
    CHECK(*fx.configured == "文风魂:明清白话");
    CHECK(fx.session.name == "wenge");
    CHECK(fx.session.content == "文风魂:明清白话");
}

TEST_CASE("锁定后 /soul <新内容>:只保存默认,快照与 revision 不动") {
    SoulCommandFixture fx("locked-write");
    fx.Run("锁定前的魂");
    fx.session.locked = true;  // 首请求已发出(锁定边界见 agent 册)
    const int revision = fx.session.revision;

    fx.Run("锁定后改的默认魂");
    CHECK(fx.configured_name == "default");
    CHECK(*fx.configured == "锁定后改的默认魂");
    // 当前快照一字不动:名称、正文、revision 都不变。
    CHECK(fx.session.name == "default");
    CHECK(fx.session.content == "锁定前的魂");
    CHECK(fx.session.revision == revision);
}

TEST_CASE("锁定后 /soul off:下个新会话关闭,当前仍保留原魂") {
    SoulCommandFixture fx("locked-off");
    fx.Run("锁定前的魂");
    fx.session.locked = true;

    fx.Run("off");
    CHECK(fx.configured_name == "off");
    CHECK(fx.configured->empty());
    CHECK(fx.session.name == "default");
    CHECK(fx.session.content == "锁定前的魂");
}

TEST_CASE("写入失败整体回滚:内存与磁盘都不留半笔新账") {
    SoulCommandFixture fx("fail");
    fx.Run("第一份魂");
    const std::string configured_before = *fx.configured;
    const std::string name_before = fx.configured_name;
    const SessionSoulSnapshot session_before = fx.session;

    // 把 SOUL.md 造成目录:写必败(跨平台)。
    const auto luban_dir = std::filesystem::path(config::HomeLubancodeDir().value());
    std::error_code ec;
    std::filesystem::remove(luban_dir / "SOUL.md", ec);
    REQUIRE(std::filesystem::create_directory(luban_dir / "SOUL.md", ec));

    fx.Run("写不进去的新魂");
    // 失败不宣称成功:configured 与草稿全没动。
    CHECK(*fx.configured == configured_before);
    CHECK(fx.configured_name == name_before);
    CHECK(fx.session == session_before);
}

TEST_CASE("多会话并存不串值:同默认账下两只快照各自独立") {
    SoulCommandFixture fx("multi");
    fx.Run("会话共享草稿起点");
    // 第二只会话的快照(同一起点拷贝,SessionStack 各建各的)。
    SessionSoulSnapshot session_b = fx.session;

    fx.Run("会话一改了自己的魂");
    CHECK(fx.session.content == "会话一改了自己的魂");
    CHECK(session_b.content == "会话共享草稿起点");  // 会话二不跟着动
    CHECK(session_b.revision == 0);
    CHECK(fx.session.revision == 1);
}
