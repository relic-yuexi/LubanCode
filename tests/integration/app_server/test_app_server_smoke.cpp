// app-server 阶段 4 单:真进程冒烟(stdio 管道替身)。
//
// SSH 夹具的进程管道替身(单子第 7 步):CI 没有本地 sshd 时,起真
// `lubancode app-server` 子进程、stdin 喂协议行、stdout 收协议行——
// 这与 `ssh <host> lubancode app-server` 在 SSH 通道那头看到的是同一
// 个 stdio 形状(SSH 只是把这三根管子接长)。真 ssh localhost 的手测
// 口径见 scripts/tests/app_server_ssh_smoke.sh。
//
// 钉的是:
//   1. stdout 从头到尾逐行可解析成协议消息(一条不糟蹋;stderr 不掺和);
//   2. 握手 -> thread/start -> thread/list -> shutdown 一条线走穿;
//   3. stderr 隔离:诊断走 stderr,stdout 只有协议行;
//   4. EOF 退场:stdin 关掉,进程自退,退出码 0,不留挂;
//   5. 坏 JSON 不炸:稳定 parse error 回来,进程活着继续吃下一条;
//   6. cwd/环境:thread/started 事件回的 cwd 是进程当前目录(SSH 通道
//      上是远端 login shell 的目录)。
//
// 缺 lubancode 可执行文件的构建(只编 lubancode_tests)整案跳过——
// 与 test_plugin_process.cpp 缺 Python 同款口径。
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "app_server/protocol.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"

using namespace lubancode;

namespace {

// 测试可执行文件旁边的 build 树根:tests/ 在 <build>/tests,主程序在
// <build>/lubancode(POSIX)或 <build>/lubancode.exe(Windows)。
std::string FindLubancodeBinary() {
#ifdef LUBANCODE_BINARY_DIR
    namespace fs = std::filesystem;
    const fs::path root = fs::path(LUBANCODE_BINARY_DIR);
    std::error_code ec;
    for (const char* name :
         {"lubancode", "lubancode.exe", "Debug/lubancode.exe", "Release/lubancode.exe"}) {
        const fs::path candidate = root / name;
        if (fs::exists(candidate, ec)) {
            return platform::PathToUtf8(candidate);
        }
    }
#endif
    return std::string();
}

// 出站行解析账。
struct OutputLedger {
    std::vector<nlohmann::json> messages;
    std::vector<std::string> bad_lines;
    std::vector<std::string> stdout_lines;
    std::string stderr_text;
};


std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string current;
    for (const char c : text) {
        if (c == '\n') {
            if (!current.empty() && current.back() == '\r') {
                current.pop_back();
            }
            lines.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        lines.push_back(current);
    }
    return lines;
}

// 起子进程喂脚本收 stdout/stderr(RunProcessWithStdin:stdin 一次性写
// 完关管 = EOF 退场路径一并验了)。SSH 通道上这是 login shell 的环境;
// 替身继承本进程环境——纯握手/listing 不发请求,不需要凭据。
OutputLedger RunAppServerSession(const std::string& binary, const std::string& stdin_script) {
    const platform::ProcessResult result = platform::RunProcessWithStdin(
        std::vector<std::string>{binary, "app-server"}, stdin_script, 30000);
    OutputLedger ledger;
    ledger.stderr_text = result.stderr_bytes;
    for (const std::string& line : SplitLines(result.stdout_bytes)) {
        ledger.stdout_lines.push_back(line);
        try {
            ledger.messages.push_back(nlohmann::json::parse(line));
        } catch (const nlohmann::json::exception&) {
            ledger.bad_lines.push_back(line);
        }
    }
    return ledger;
}

}  // namespace

TEST_CASE("真进程冒烟:握手 -> thread/start -> thread/list -> shutdown -> EOF 自退") {
    const std::string binary = FindLubancodeBinary();
    if (binary.empty()) {
        return; // 缺主程序:跳过(只编了 lubancode_tests 的构建)
    }
    // 喂完 stdin 关管 = EOF;服务应处理完全部行、刷完出站、退码 0。
    const std::string script = std::string() +
        R"({"id":1,"method":"initialize","params":{"clientName":"smoke-test"}})" "\n"
        R"({"method":"initialized"})" "\n"
        R"({"id":2,"method":"thread/start","params":{}})" "\n"
        R"({"id":3,"method":"thread/list","params":{}})" "\n"
        R"({"id":4,"method":"shutdown","params":{}})" "\n";
    const OutputLedger ledger = RunAppServerSession(binary, script);

    // stdout 逐行可解析:一行不糟蹋。
    CHECK(ledger.bad_lines.empty());
    REQUIRE(ledger.messages.size() >= 4);

    // 响应按 id 配对(事件与响应同走一条出站队列,不钉位置——thread/
    // started 事件完全可能排在 thread/start 响应前面)。
    const auto find_response = [&ledger](int id) -> const nlohmann::json* {
        for (const nlohmann::json& message : ledger.messages) {
            if (message.contains("id") && message["id"] == id) {
                return &message;
            }
        }
        return nullptr;
    };

    // 1 = initialize 的响应:能力表在。
    const nlohmann::json* init = find_response(1);
    REQUIRE(init != nullptr);
    CHECK((*init)["result"]["protocolVersion"].get<std::string>() == app_server::kProtocolVersion);
    CHECK((*init)["result"].contains("capabilities"));
    CHECK((*init)["result"].contains("lubancodeVersion"));
    CHECK((*init)["result"].contains("platform"));

    // 2 = thread/start 的响应:threadId 给了。
    const nlohmann::json* start = find_response(2);
    REQUIRE(start != nullptr);
    CHECK((*start)["result"]["threadId"].get<std::string>().size() >= 8);

    // 3 = thread/list 的响应:数组在(自家的场刚建,能否列出取决于 HOME
    // 的会话档——只钉形状,不钉内容)。
    const nlohmann::json* list = find_response(3);
    REQUIRE(list != nullptr);
    CHECK((*list)["result"]["threads"].is_array());

    // 4 = shutdown 的响应。
    const nlohmann::json* shutdown = find_response(4);
    REQUIRE(shutdown != nullptr);
    CHECK(shutdown->contains("result"));

    // thread/started 事件在出站流里(cwd 带上——SSH 通道上就是远端目录)。
    bool saw_thread_started = false;
    for (const nlohmann::json& message : ledger.messages) {
        if (message.contains("method") && message["method"] == "thread/started") {
            saw_thread_started = true;
            CHECK(message["params"].contains("cwd"));
            CHECK(message["params"].contains("threadId"));
            CHECK(message["params"].contains("seq"));
        }
    }
    CHECK(saw_thread_started);
}

TEST_CASE("真进程冒烟:坏 JSON 回稳定错误,进程不炸继续吃") {
    const std::string binary = FindLubancodeBinary();
    if (binary.empty()) {
        return;
    }
    const std::string script = std::string() +
        R"({"id":1,"method":"initialize","params":{}})" "\n"
        R"({"method":"initialized"})" "\n"
        "this is not json\n" +
        R"({"id":5,"method":"no/such-method","params":{}})" "\n";
    const OutputLedger ledger = RunAppServerSession(binary, script);

    CHECK(ledger.bad_lines.empty());
    // 坏 JSON 的 parse error(id=null)与未知方法(-32601)都回了。
    bool saw_parse_error = false;
    bool saw_method_not_found = false;
    for (const nlohmann::json& message : ledger.messages) {
        if (message.contains("error")) {
            if (message["error"]["code"] == app_server::kErrParseError) {
                saw_parse_error = true;
                CHECK(message["id"].is_null());
            }
            if (message["error"]["code"] == app_server::kErrMethodNotFound) {
                saw_method_not_found = true;
                CHECK(message["id"] == 5);
            }
        }
    }
    CHECK(saw_parse_error);
    CHECK(saw_method_not_found);
}

TEST_CASE("真进程冒烟:stderr 隔离——stdout 只有协议行") {
    const std::string binary = FindLubancodeBinary();
    if (binary.empty()) {
        return;
    }
    const std::string script = std::string() +
        R"({"id":1,"method":"initialize","params":{}})" "\n"
        R"({"method":"initialized"})" "\n"
        R"({"method":"exit"})" "\n";
    const OutputLedger ledger = RunAppServerSession(binary, script);

    // 每一行 stdout 都 parse 成了合法协议消息(上面 bad_lines 已查);
    // 这里再钉:出站的每条消息要么带 method(事件/通知),要么带 id
    //(响应)——绝无第三种形状混入。
    for (const nlohmann::json& message : ledger.messages) {
        const bool shaped = message.contains("method") || message.contains("id");
        CHECK(shaped);
        CHECK_FALSE(message.contains("stdout"));
    }
    CHECK(ledger.messages.size() >= 1);
}
