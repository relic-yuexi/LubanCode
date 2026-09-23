// TUI 排版批 7:channel 三件 CLI(pairing/setup/status)的输出形状册(一册)。
//   - pairing:成功回执键值对框(RenderPairingReceipt 纯函数),批准走
//     Pass 语义色(table_pass,批 4 裁量);
//   - setup:头部信息块键值对框(RenderChannelSetupBanner 纯函数,横幅句
//     做标题);未知平台守门 stderr 原样 + 退 1;
//   - status:三节排版(RenderChannelStatusView)——行文本由三个纯构造器
//     定(旧册钉的数据面,一字不动),这里喂真 builder 的输出断拆列形状;
//   - plain 零转义、无框;dark 有框角;
//   - --json 分支机器面 printf 原样,本册不碰。
//
// setup 的向导问答全程要真交互终端(StdinIsTerminal 守门),CI 里测不到
// 交互路径——头部块抽纯函数钉形状,向导全程真机未验。

#include <doctest/doctest.h>

#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "cli/channel_pairing_command.hpp"
#include "cli/channel_setup_command.hpp"
#include "cli/channel_status_command.hpp"
#include "cli/terminal_port.hpp"
#include "cli/theme.hpp"

using namespace lubancode;
using namespace lubancode::cli;

namespace {

class OutputCapture {
public:
    // 双流都截:setup 守门断 stderr。
    OutputCapture() { cli::TermPort().Redirect(&out_, &err_); }
    ~OutputCapture() { cli::TermPort().Reset(); }
    std::string out() const { return out_.str(); }  // 按值:str() 是临时,绑引用即悬垂
    std::string err() const { return err_.str(); }

private:
    std::ostringstream out_;
    std::ostringstream err_;
};

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

std::string Join(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& line : lines) {
        out += line;
        out += "\n";
    }
    return out;
}

const cli::Theme dark = cli::BuiltinTheme("dark");
const cli::Theme plain = cli::BuiltinTheme("plain");

constexpr const char* kBoxTopLeft = "\xe2\x94\x8c";  // ┌

}  // namespace

// ---- pairing ---------------------------------------------------------------

TEST_CASE("channel pairing 回执:键值对框,批准走 table_pass 语义色") {
    const std::vector<std::string> lines = cli::RenderPairingReceipt(
        "approve", "qqbot", "main", "sender-abc", dark, /*width=*/0);
    const std::string out = Join(lines);

    CHECK(Contains(out, "channel pairing"));  // 标题嵌上边框
    CHECK(Contains(out, kBoxTopLeft));
    CHECK(Contains(out, "已批准"));
    CHECK(Contains(out, "qqbot/main"));
    CHECK(Contains(out, "sender-abc"));
    CHECK(Contains(out, "提醒"));  // 第二句拆列
    CHECK(Contains(out, dark.table_pass));  // 批准 = Pass(table_pass,批 4 裁量)
}

TEST_CASE("channel pairing 回执:拒绝不上 Pass 色;plain 零转义无框") {
    const std::vector<std::string> reject_lines =
        cli::RenderPairingReceipt("reject", "qqbot", "main", "sender-abc", dark, /*width=*/0);
    CHECK(Join(reject_lines).find(dark.table_pass) == std::string::npos);

    const std::vector<std::string> plain_lines =
        cli::RenderPairingReceipt("approve", "qqbot", "main", "sender-abc", plain, /*width=*/0);
    const std::string out = Join(plain_lines);
    CHECK(out.find("\x1b") == std::string::npos);
    CHECK(out.find(kBoxTopLeft) == std::string::npos);
    CHECK(Contains(out, "已批准"));
}

// ---- setup -----------------------------------------------------------------

TEST_CASE("channel setup 头部:键值对框,横幅句做标题,三句拆列") {
    const std::vector<std::string> lines = cli::RenderChannelSetupBanner(
        "QQ 机器人", "qqbot", "main", "D:/home/.lubancode/config.json",
        "D:/home/.lubancode/secrets", plain, /*width=*/0);
    const std::string out = Join(lines);

    CHECK(out.find("\x1b") == std::string::npos);
    CHECK(Contains(out, "渠道配置向导 —— QQ 机器人"));  // 标题(现成横幅句)
    CHECK(Contains(out, "目标账号"));
    CHECK(Contains(out, "qqbot/main"));
    CHECK(Contains(out, "配置文件"));
    CHECK(Contains(out, "D:/home/.lubancode/config.json"));
    CHECK(Contains(out, "受管密钥目录"));
    CHECK(out.find(kBoxTopLeft) == std::string::npos);  // plain 无框
}

TEST_CASE("channel setup 头部:dark 有框角") {
    const std::vector<std::string> lines = cli::RenderChannelSetupBanner(
        "QQ 机器人", "qqbot", "main", "config.json", "secrets", dark, /*width=*/0);
    CHECK(Contains(Join(lines), kBoxTopLeft));
}

TEST_CASE("channel setup:未知平台守门,退 1,stdout 无 frame 无转义") {
    // 错误行走 fprintf(stderr) 直写 C 流(批 7 不动错误流),TermPort 的
    // Redirect 截不到——文字未动由 diff 核实(PR body),这里钉退出码与
    // stdout 面。
    ChannelSetupCommandArgs args;
    args.platform = "no-such-channel";
    OutputCapture capture;
    const int code = RunChannelSetupCommand(args);
    CHECK(code == 1);
    CHECK(capture.out().find("\x1b") == std::string::npos);
}

// ---- status ----------------------------------------------------------------

TEST_CASE("channel status:四步状态逐句拆列,键值两列对齐") {
    // 喂真构造器的输出(数据面旧册钉着,这里只管排版)。
    ChannelFourStateInput input;
    input.account_configured = true;
    input.online = true;
    input.online_detail = "boot b-1";
    input.pairing_parse_ok = true;
    input.pairing_approved = 2;
    input.model_configured = true;
    input.recent_turn_present = true;
    input.recent_turn_ok = true;
    input.recent_turn_sid = 7;
    input.recent_turn_at_ms = 1000;
    const ChannelFourStateView four = BuildChannelFourState("qqbot", "main", input);

    const std::vector<std::string> verdict = {
        "qqbot/main: 没有连接状态快照(Gateway 未运行,或该账号未装配)"};

    const std::vector<std::string> lines = cli::RenderChannelStatusView(
        "qqbot", "main", four.lines, verdict, {}, plain, /*width=*/0);
    const std::string out = Join(lines);

    CHECK(out.find("\x1b") == std::string::npos);
    CHECK(Contains(out, "qqbot/main 状态"));  // 头句(剥冒号)做标题
    CHECK(Contains(out, "1. 配置已存"));
    CHECK(Contains(out, "是"));  // 拆列后的值
    CHECK(Contains(out, "2. QQ 在线"));
    CHECK(Contains(out, "boot b-1"));
    CHECK(Contains(out, "连接明细"));  // 第二节标题
    CHECK(Contains(out, "没有连接状态快照"));
}

TEST_CASE("channel status:来信链首行剥尾冒号做列表标题,行整行进列") {
    ChannelRecentChainInput chain_input;
    chain_input.ledger_present = true;
    channel::ChannelIngressRecentEntry entry;
    entry.sid = 4;
    entry.state = "dead_letter";
    entry.reason = "turn_failed";
    entry.received_at_ms = 0;
    chain_input.ingress.push_back(entry);
    const ChannelRecentChainView chain = BuildChannelRecentChain(chain_input, 8);

    const std::vector<std::string> lines =
        cli::RenderChannelStatusView("qqbot", "main", {}, {}, chain.lines, plain, /*width=*/0);
    const std::string out = Join(lines);

    CHECK(out.find("\x1b") == std::string::npos);
    CHECK(Contains(out, "最近来信链"));   // 标题(尾冒号剥掉)
    CHECK(!Contains(out, "条):"));       // 引导句的尾冒号不再原样出现
    CHECK(Contains(out, "sid=4"));
    CHECK(Contains(out, "dead_letter"));
}

TEST_CASE("channel status:来信链空态单句进键值对,不造空列表") {
    ChannelRecentChainInput chain_input;  // ledger_present=false
    const ChannelRecentChainView chain = BuildChannelRecentChain(chain_input, 8);

    const std::vector<std::string> lines =
        cli::RenderChannelStatusView("qqbot", "main", {}, {}, chain.lines, plain, /*width=*/0);
    const std::string out = Join(lines);
    CHECK(Contains(out, "还没有来信账"));
    CHECK(out.find("\x1b") == std::string::npos);
}

TEST_CASE("channel status:dark 三节有框角") {
    ChannelFourStateInput input;  // 全空 = 四步全否,行最多
    const ChannelFourStateView four = BuildChannelFourState("qqbot", "main", input);

    const std::vector<std::string> lines =
        cli::RenderChannelStatusView("qqbot", "main", four.lines, {}, {}, dark, /*width=*/0);
    CHECK(Contains(Join(lines), kBoxTopLeft));
}
