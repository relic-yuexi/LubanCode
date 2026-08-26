// 宽窄转换异常单的回归:三组对应规格"验收"节——
//   1. wire 边界:Utf8DeltaGate 与 AgentLoop 的劈半 delta(四字节 emoji 在
//      SSE 块边界拦腰斩)——显示层只见完整合法 UTF-8,history 自愈拼齐;
//   2. path 边界:PathToUtf8/Utf8ToPath 对 emoji 路径往返(GBK 机器上
//      path::string() 对同一抛 1113,真机探针已实锤,这里钉住新通道);
//   3. 顶层兜底:后端流中途抛 std::exception,RunTurn 收口成 status=1,
//      不许异常穿透把进程掀了。
// 另有 platform::WideToUtf8/Utf8ToWide 的"不许抛"合同(坏字符替换
// U+FFFD)——Windows 专属,条件编译。

#include <doctest/doctest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "agent/loop.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "app/turn_runner.hpp"
#include "cli/context_tracker.hpp"
#include "cli/theme.hpp"
#include "cli/transcript.hpp"
#include "platform/paths.hpp"
#include "platform/text_encoding.hpp"
#include "tools/registry.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

using namespace lubancode;

namespace {

constexpr const char* kEmoji = "\xF0\x9F\x93\x9A";  // 📚(四字节)
constexpr const char* kReplacement = "\xEF\xBF\xBD";  // U+FFFD

class StreamCapture {
public:
    explicit StreamCapture(std::ostream& stream) : stream_(stream), old_buffer_(stream.rdbuf(buffer_.rdbuf())) {}
    ~StreamCapture() { stream_.rdbuf(old_buffer_); }

    StreamCapture(const StreamCapture&) = delete;
    StreamCapture& operator=(const StreamCapture&) = delete;

    std::string str() const { return buffer_.str(); }

private:
    std::ostream& stream_;
    std::ostringstream buffer_;
    std::streambuf* old_buffer_;
};

// 按脚本吐事件的假后端(test_loop.cpp 同款,这里要的是劈半 delta 脚本)。
class ScriptBackend : public api::Backend {
public:
    std::vector<std::vector<api::StreamEvent>> scripts;

    std::expected<void, api::Error> send_stream(
        const api::Request&,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* = nullptr) override {
        if (scripts.empty()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "脚本用完了", 0});
        }
        std::vector<api::StreamEvent> script = std::move(scripts.front());
        scripts.erase(scripts.begin());
        for (const auto& event : script) {
            on_event(event);
        }
        return {};
    }
};

// send_stream 抛 std::runtime_error 的后端:what() 就用真机上那条 1113
// 文案,复刻"宽窄转换异常掐死会话"的现场。
class ThrowingBackend : public api::Backend {
public:
    std::expected<void, api::Error> send_stream(
        const api::Request&,
        const std::function<void(const api::StreamEvent&)>&,
        const std::atomic<bool>* = nullptr) override {
        throw std::runtime_error(
            "No mapping for the Unicode character exists in the target multi-byte code page.");
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// 1. Utf8DeltaGate:劈半序列扣住拼齐,坏字节立即替换,残尾 Flush 放完
// ---------------------------------------------------------------------------

TEST_CASE("Utf8DeltaGate:四字节 emoji 劈三块,拼齐再放行") {
    platform::Utf8DeltaGate gate;
    // "好" + emoji 首字节:放行 "好",扣住 \xF0。
    CHECK(gate.Feed(std::string("好") + "\xF0") == "好");
    CHECK(gate.pending_bytes() == 1);
    // 中间两块续字节:整块扣住,一个字节都不放。
    CHECK(gate.Feed("\x9F\x93").empty());
    CHECK(gate.pending_bytes() == 3);
    // 收尾字节到齐:整个 emoji 放行。
    const std::string tail = gate.Feed(std::string(kEmoji).substr(3) + "!");
    CHECK(tail == std::string(kEmoji) + "!");
    CHECK(gate.pending_bytes() == 0);
}

TEST_CASE("Utf8DeltaGate:彻底非法字节立即替换,不许扣住不放") {
    platform::Utf8DeltaGate gate;
    // \xFF 非法首字节、\xC0 过长编码首字节:当场替换 U+FFFD,后面的好字照放。
    const std::string out = gate.Feed(std::string("\xFF") + "\xC0" + "ok");
    CHECK(out == std::string(kReplacement) + kReplacement + "ok");
    CHECK(gate.pending_bytes() == 0);
}

TEST_CASE("Utf8DeltaGate:续字节凑不齐就是坏序列,替换放行") {
    platform::Utf8DeltaGate gate;
    // \xE4\xB8 声明三字节序列却只有两字节,下一块来的是新字符:整段判坏。
    CHECK(gate.Feed("\xE4\xB8").empty());
    const std::string out = gate.Feed("A");
    CHECK(out == std::string(kReplacement) + kReplacement + "A");
}

TEST_CASE("Utf8DeltaGate:流收尾 Flush 把残尾按 U+FFFD 放完") {
    platform::Utf8DeltaGate gate;
    CHECK(gate.Feed("x") == "x");
    CHECK(gate.Feed("\xF0\x9F").empty());
    const std::string flushed = gate.Flush();
    CHECK(flushed == std::string(kReplacement) + kReplacement);
    CHECK(gate.pending_bytes() == 0);
    CHECK(gate.Flush().empty());  // 再 Flush 是空操作
}

TEST_CASE("Utf8DeltaGate:合法文本原样透传") {
    platform::Utf8DeltaGate gate;
    const std::string text = std::string("中文与 ") + kEmoji + " 混排\n第二行";
    CHECK(gate.Feed(text) == text);
    CHECK(gate.Flush().empty());
}

// 截短对齐码点边界的两把尺(截断把合法内容砍出非法 UTF-8 的回归:
// 请求体 dump 当场 type_error.316,会话每回合必挂)。
TEST_CASE("Utf8PrefixBoundary:前缀刀口退到多字节序列开头之前") {
    const std::string text = "abc你好";  // 3 ASCII + 2 个三字节汉字(共 9 字节)
    CHECK(platform::Utf8PrefixBoundary(text, 0) == 0);
    CHECK(platform::Utf8PrefixBoundary(text, 3) == 3);   // 落在 ASCII/序列头,原样
    CHECK(platform::Utf8PrefixBoundary(text, 4) == 3);   // 劈在"你"的腰上,退到 3
    CHECK(platform::Utf8PrefixBoundary(text, 5) == 3);
    CHECK(platform::Utf8PrefixBoundary(text, 6) == 6);   // "你"完整,"好"的开头
    CHECK(platform::Utf8PrefixBoundary(text, 8) == 6);
    CHECK(platform::Utf8PrefixBoundary(text, 9) == 9);   // 末尾
    CHECK(platform::Utf8PrefixBoundary(text, 100) == 9);  // 越界收口到 size
    CHECK(platform::Utf8PrefixBoundary("", 5) == 0);      // 空串安全
    // 四字节 emoji 同样不劈。
    const std::string with_emoji = std::string("x") + kEmoji + "y";
    CHECK(platform::Utf8PrefixBoundary(with_emoji, 3) == 1);  // 劈在 emoji 中间,退到 1
    // 出口前缀一定合法。
    for (std::size_t i = 0; i <= text.size(); ++i) {
        CHECK(platform::IsValidUtf8(text.substr(0, platform::Utf8PrefixBoundary(text, i))));
    }
}

TEST_CASE("Utf8SuffixBoundary:尾段起点推过悬着的续字节") {
    const std::string text = "abc你好";
    CHECK(platform::Utf8SuffixBoundary(text, 0) == 0);
    CHECK(platform::Utf8SuffixBoundary(text, 3) == 3);
    CHECK(platform::Utf8SuffixBoundary(text, 4) == 6);  // 悬在"你"的腰上,推到"好"
    CHECK(platform::Utf8SuffixBoundary(text, 5) == 6);
    CHECK(platform::Utf8SuffixBoundary(text, 6) == 6);
    CHECK(platform::Utf8SuffixBoundary(text, 9) == 9);
    CHECK(platform::Utf8SuffixBoundary(text, 100) == 9);  // 越界收口到 size
    CHECK(platform::Utf8SuffixBoundary("", 3) == 0);
    // 出口尾段一定合法。
    for (std::size_t i = 0; i <= text.size(); ++i) {
        CHECK(platform::IsValidUtf8(text.substr(platform::Utf8SuffixBoundary(text, i))));
    }
}

// ---------------------------------------------------------------------------
// 1b. AgentLoop 集成:劈半 delta 流,显示回调只见完整 UTF-8,history 拼齐
// ---------------------------------------------------------------------------

TEST_CASE("AgentLoop:劈半 emoji 的 delta 流,回调每段合法、history 自愈") {
    ScriptBackend backend;
    backend.scripts.push_back({
        api::MessageStart{"msg_1", "model"},
        api::TextDelta{"开头好"},
        api::TextDelta{std::string(kEmoji).substr(0, 2)},  // 劈半:前两字节
        api::TextDelta{std::string(kEmoji).substr(2)},     // 后两字节
        api::TextDelta{"收尾"},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    });

    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry, "test-model", "system", std::nullopt, 5, 200000);

    std::vector<std::string> shown;
    agent::Callbacks callbacks;
    callbacks.on_text_delta = [&shown](const std::string& text) { shown.push_back(text); };

    const auto result = loop.Run("劈半流", callbacks, nullptr);
    REQUIRE(result.has_value());

    // 显示层:每一段都是完整合法 UTF-8,没有任何一段以半个字收尾。
    std::string joined;
    for (const std::string& piece : shown) {
        CHECK(platform::IsValidUtf8(piece));
        joined += piece;
    }
    CHECK(joined == std::string("开头好") + kEmoji + "收尾");

    // history:文本块拼齐了完整 emoji(劈半自愈)。
    const auto& history = loop.history();
    REQUIRE(history.size() == 2);
    REQUIRE(history[1].content.size() == 1);
    const auto* text = std::get_if<api::TextBlock>(&history[1].content[0]);
    REQUIRE(text != nullptr);
    CHECK(text->text == std::string("开头好") + kEmoji + "收尾");
}

// ---------------------------------------------------------------------------
// 2. path 边界:PathToUtf8/Utf8ToPath 与真 emoji 文件
// ---------------------------------------------------------------------------

TEST_CASE("PathToUtf8/Utf8ToPath:emoji 路径往返不走 ACP 窄口") {
    const std::string utf8_dir = "lubancode_unicode_test_\xF0\x9F\x93\x9A";
    const std::string temp_root = platform::PathToUtf8(std::filesystem::temp_directory_path());
    const std::filesystem::path dir = platform::Utf8ToPath(temp_root + "/" + utf8_dir);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    REQUIRE_FALSE(ec);

    // 文件名也得走 u8 通道:dir / "窄串" 的斜杠拼接在 Windows 上按 ACP 解
    // 码,GBK 机器上 UTF-8 文件名字节当场变乱码(这行测试最初就栽在这)。
    const std::string file_name = std::string("\xE7\xAC\x94\xE8\xAE\xB0_") + kEmoji + ".txt";
    const std::filesystem::path file = platform::Utf8ToPath(platform::PathToUtf8(dir) + "/" + file_name);
    {
        std::ofstream out(file, std::ios::binary);
        out << "x";
    }

    // u8 通道往返:字节级还原。
    CHECK(platform::PathToUtf8(file) == platform::PathToUtf8(dir) + "/\xE7\xAC\x94\xE8\xAE\xB0_" +
                                             std::string(kEmoji) + ".txt");

#ifdef _WIN32
    // GBK(ACP 936)机器上的病灶钉子:老通道 path::string() 对同一目录抛的
    // 正是那条 1113 文案(真机探针实锤)。别的代码页下不 assert,免得
    // 把测试绑死在一种系统配置上。
    if (GetACP() == 936) {
        bool threw = false;
        try {
            (void)file.string();
        } catch (const std::system_error&) {
            threw = true;
        }
        CHECK(threw);
    }
#endif

    std::filesystem::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// 3. 宽窄转换不许抛(Windows)
// ---------------------------------------------------------------------------

#ifdef _WIN32

TEST_CASE("WideToUtf8:孤立代理对替换 U+FFFD,不失败不抛") {
    std::wstring with_lone;
    with_lone.push_back(L'a');
    with_lone.push_back(0xD83D);  // 孤立高代理
    with_lone.push_back(0xDC00);  // 孤立低代理
    with_lone.push_back(L'b');
    const std::string out = platform::WideToUtf8(with_lone);
    CHECK_FALSE(out.empty());
    CHECK(platform::IsValidUtf8(out));
    // 两侧的好字符都保得住。
    CHECK(out.front() == 'a');
    CHECK(out.back() == 'b');

    // 成对代理(真 emoji)原样编回 UTF-8。
    std::wstring emoji;
    emoji.push_back(0xD83D);
    emoji.push_back(0xDCDA);
    CHECK(platform::WideToUtf8(emoji) == kEmoji);
}

TEST_CASE("Utf8ToWide:坏字节尽力替换,不失败不抛") {
    const std::wstring wide = platform::Utf8ToWide(std::string("好") + "\xFF\xC0");
    CHECK_FALSE(wide.empty());
    CHECK(wide.front() == 0x597D);  // "好"完好运回
}

#endif  // _WIN32

// ---------------------------------------------------------------------------
// 4. 顶层兜底:后端中途抛异常,回合收口 status=1,不穿透
// ---------------------------------------------------------------------------

TEST_CASE("RunTurn:后端抛 1113 异常,回合按失败收口,不掀进程") {
    ThrowingBackend backend;
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry, "test-model", "system", std::nullopt, 5, 200000);

    lubancode::cli::Theme theme;  // plain,无 ANSI
    lubancode::cli::ContextTracker context_tracker(1000);
    std::set<std::string> always_allowed;
    std::vector<lubancode::cli::TranscriptItem> transcript;

    lubancode::app::RunTurnResult out;
    std::string rendered;
    {
        StreamCapture stdout_capture(std::cout);
        StreamCapture stderr_capture(std::cerr);
        lubancode::app::TurnContext turn;
        turn.loop = &loop;
        turn.user_input = "问 LIS 的 nlogn 做法";
        turn.auto_confirm = true;
        turn.always_allowed_tools = &always_allowed;
        turn.theme = theme;
        turn.context_tracker = &context_tracker;
        turn.registry = &registry;
        turn.is_console = false;
        turn.transcript = &transcript;
        out = lubancode::app::RunTurn(std::move(turn));
        rendered = stdout_capture.str() + stderr_capture.str();
    }
    CHECK(out.status == 1);
    CHECK_FALSE(out.cancelled);
    CHECK(rendered.find("No mapping for the Unicode character") != std::string::npos);
    // 用户消息已入 history(落盘/resume 的账都在)。
    REQUIRE_FALSE(loop.history().empty());
    CHECK(loop.history().front().role == api::Role::User);
}
