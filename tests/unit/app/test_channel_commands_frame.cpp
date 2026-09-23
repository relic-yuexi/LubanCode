// TUI 排版批 5a(tui优化todo.todo P5:/channel 全族)的输出形状册。
//   - 总览:配置表(channel/account/state/dm/group/secret/transport)+ 运行
//     态表(account/state/gen/retry_at/inbox/dead_letter/lock);state 列
//     enabled/disabled 与运行态各按 pass/skip/fail 分档;
//   - show 键值对框(标题=渠道/账号定位串)+ 迁移表;doctor 键值对框,密钥
//     四档 pass/error 语义色;pairing list 走列表(hint 挂操作提示);
//   - handler 的用法/引导/失败反馈全进 frame,不再裸打印;
//   - plain 主题(--no-color/T3 降级路径)钉零转义字节、无框字形——合同
//     第 3 条;
//   - /channel 无 --json/机器消费分支(源码核实),字节级不变一条天然满足。
//
// Format* 是纯函数直调;handler 分支(HandleSlashChannel)经手造窄 context
// 走真函数。断言只看形状与相对位置,不看绝对宽度(测试进程探不到终端,
// 框宽按内容自适应;真机宽度由主人会话内连敲验收)。

#include <doctest/doctest.h>

#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "app/commands/channel_commands.hpp"
#include "channel/manager.hpp"
#include "cli/line_editor.hpp"  // DisplayWidthUtf8:对齐断言按显示列量
#include "cli/terminal_port.hpp"
#include "cli/theme.hpp"

using namespace lubancode;
using namespace lubancode::app;
using namespace lubancode::channel;

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

int DisplayColOf(const std::string& row, const std::string& needle) {
    const std::size_t at = row.find(needle);
    if (at == std::string::npos) {
        return -1;
    }
    return static_cast<int>(cli::DisplayWidthUtf8(row.substr(0, at)));
}

constexpr const char* kBoxLightTopLeft = "\xe2\x94\x8c";  // ┌
constexpr const char* kBoxLightVert = "\xe2\x94\x82";     // │

ChannelManager::AccountSnapshot MakeSnapshot(ChannelAccountState state) {
    ChannelManager::AccountSnapshot snapshot;
    snapshot.channel_id = "qqbot";
    snapshot.account_id = "main";
    snapshot.state = state;
    snapshot.generation = 2;
    snapshot.inbox_pending = 3;
    snapshot.dead_letter_count = 1;
    snapshot.pairing_pending = 2;
    snapshot.pairing_approved = 1;
    snapshot.lock_held = true;
    snapshot.dm_policy = DmPolicy::Pairing;
    snapshot.group_policy = GroupPolicy::Allowlist;
    snapshot.credential = CredentialSource::FromEnv;
    AccountStatusTransition transition;
    transition.from = ChannelAccountState::Connecting;
    transition.to = ChannelAccountState::Running;
    snapshot.recent_transitions.push_back(transition);
    return snapshot;
}

std::map<std::string, ChannelUserConfig> MakeChannels() {
    std::map<std::string, ChannelUserConfig> channels;
    ChannelUserConfig channel;
    channel.enabled = true;
    channel.default_account = "main";
    ChannelAccountUserConfig account;
    account.enabled = true;
    account.secret_env = "QQBOT_SECRET";
    channel.accounts.emplace("main", account);
    channels.emplace("qqbot", channel);
    return channels;
}

const cli::Theme dark = cli::BuiltinTheme("dark");
const cli::Theme plain = cli::BuiltinTheme("plain");

// handler 直调:窄 context 全空指针(普通交互形态),theme 注入。
std::string RunChannel(const std::string& args, const cli::Theme& theme) {
    ChannelCommandContext ctx;
    ctx.theme = &theme;
    const cli::ParsedSlashCommand parsed{cli::SlashCommand::Channel, args, "channel", std::string()};
    OutputCapture capture;
    HandleSlashChannel(ctx, parsed);
    return capture.text();
}

}  // namespace

TEST_CASE("总览:配置表 + 尾注框,state 列 enabled 走 pass 档") {
    const auto channels = MakeChannels();
    const auto lines = FormatChannelsOverview(&channels, nullptr, dark);
    std::string plain_text;
    for (const std::string& line : lines) {
        plain_text += line + "\n";
    }

    // 配置表:框角 + 表头 + 账号行;尾注(gateway 引导)进键值对框。
    REQUIRE(Contains(plain_text, kBoxLightTopLeft));
    CHECK(Contains(plain_text, "channel"));
    CHECK(Contains(plain_text, "account"));
    CHECK(Contains(plain_text, "dm"));
    CHECK(Contains(plain_text, "group"));
    CHECK(Contains(plain_text, "secret"));
    CHECK(Contains(plain_text, "transport"));
    CHECK(Contains(plain_text, "main"));
    CHECK(Contains(plain_text, "gateway not running"));

    // 语义色:enabled 的账号在 state 列走 pass 档(table_pass)。
    std::string colored;
    for (const std::string& line : lines) {
        colored += line + "\n";
    }
    CHECK(Contains(colored, dark.table_pass + "[enabled]"));

    // 对齐:channel 行与 account 行的 state 单元格同列(前者 "qqbot" 后者
    // "main",列宽按最宽行对齐后 [enabled] 处处同列)。按 StripAnsi 后的
    // 文本量显示列(批 2 形状册同款——ANSI 码会劈开探针)。
    int channel_col = -1;
    int account_col = -1;
    std::istringstream align_lines(plain_text);
    std::string align_line;
    while (std::getline(align_lines, align_line)) {
        if (Contains(align_line, "qqbot")) channel_col = DisplayColOf(align_line, "[enabled]");
        if (Contains(align_line, "main")) account_col = DisplayColOf(align_line, "[enabled]");
    }
    CHECK(channel_col > 0);
    CHECK(account_col > 0);
    CHECK(channel_col == account_col);
}

TEST_CASE("总览:运行态表,running 走 pass、退避走 skip、终态走 fail") {
    std::vector<ChannelManager::AccountSnapshot> snapshots;
    snapshots.push_back(MakeSnapshot(ChannelAccountState::Running));
    snapshots.push_back(MakeSnapshot(ChannelAccountState::Backoff));
    snapshots.back().account_id = "second";
    snapshots.push_back(MakeSnapshot(ChannelAccountState::Fatal));
    snapshots.back().account_id = "third";

    const auto lines = FormatChannelsOverview(nullptr, &snapshots, dark);
    std::string plain_text;
    std::string colored;
    for (const std::string& line : lines) {
        plain_text += line + "\n";
        colored += line + "\n";
    }
    // 无配置侧:运行态照实列(标题句保留)。
    CHECK(Contains(plain_text, "以下为本进程运行态"));
    CHECK(Contains(plain_text, "运行态"));
    CHECK(Contains(plain_text, "qqbot/main"));
    CHECK(Contains(plain_text, "running"));
    CHECK(Contains(plain_text, "backoff"));
    CHECK(Contains(plain_text, "fatal"));
    // state 列三档语义色(批 5a 验收点)。
    CHECK(Contains(colored, dark.table_pass + "running"));
    CHECK(Contains(colored, dark.table_skip + "backoff"));
    CHECK(Contains(colored, dark.error + "fatal"));
}

TEST_CASE("show:键值对框带定位串标题 + 迁移表") {
    const auto snapshot = MakeSnapshot(ChannelAccountState::Running);
    const auto lines = FormatChannelShow(&snapshot, dark);
    std::string plain_text;
    for (const std::string& line : lines) {
        plain_text += line + "\n";
    }
    REQUIRE(Contains(plain_text, kBoxLightTopLeft));
    // 标题=定位串,尾冒号剥掉(批 2 裁量 3)。
    CHECK(Contains(plain_text, "渠道 qqbot / 账号 main"));
    CHECK(plain_text.find("账号 main:") == std::string::npos);
    // 键值对拆列:状态/策略/密钥来源/水位各成 key。
    CHECK(Contains(plain_text, "状态"));
    CHECK(Contains(plain_text, "dm=pairing"));
    CHECK(Contains(plain_text, "密钥来源"));
    CHECK(Contains(plain_text, "inbox=3"));
    // 迁移表:transition 列保持 "from -> to" 连排。
    CHECK(Contains(plain_text, "connecting -> running"));
}

TEST_CASE("doctor:密钥四档语义色,明文/缺失走 error 档") {
    {
        const auto snapshot = MakeSnapshot(ChannelAccountState::Running);
        const auto lines = FormatChannelDoctor(&snapshot, dark);
        std::string colored;
        for (const std::string& line : lines) {
            colored += line + "\n";
        }
        CHECK(Contains(colored, dark.table_pass + "环境变量来源。ok。"));
        CHECK(Contains(colored, "体检不发平台请求"));
    }
    {
        auto snapshot = MakeSnapshot(ChannelAccountState::Running);
        snapshot.credential = CredentialSource::InlinePlaintext;
        const auto lines = FormatChannelDoctor(&snapshot, dark);
        std::string colored;
        for (const std::string& line : lines) {
            colored += line + "\n";
        }
        CHECK(Contains(colored, dark.error + "WARNING"));
    }
    {
        auto snapshot = MakeSnapshot(ChannelAccountState::Running);
        snapshot.credential = CredentialSource::Missing;
        const auto lines = FormatChannelDoctor(&snapshot, dark);
        std::string colored;
        for (const std::string& line : lines) {
            colored += line + "\n";
        }
        CHECK(Contains(colored, dark.error + "CredentialsMissing"));
    }
}

TEST_CASE("pairing list:列表行带 sender/剩余/操作提示,空态进框") {
    {
        const std::vector<ChannelManager::PendingPairingView> no_pending;
        const auto lines =
            FormatChannelPairingList("qqbot/main", &no_pending, 1000000, dark);
        std::string plain_text;
        for (const std::string& line : lines) {
            plain_text += line + "\n";
        }
        REQUIRE(Contains(plain_text, kBoxLightTopLeft));
        CHECK(Contains(plain_text, "没有待审配对"));
    }
    {
        std::vector<ChannelManager::PendingPairingView> pending(2);
        pending[0].sender_id = "sender-a";
        pending[0].expires_at_ms = 1060000;  // 剩 60 秒(60000ms)
        pending[1].sender_id = "sender-b";
        pending[1].expires_at_ms = 900000;  // 已过期
        const auto lines = FormatChannelPairingList("qqbot/main", &pending, 1000000, dark);
        std::string plain_text;
        for (const std::string& line : lines) {
            plain_text += line + "\n";
        }
        REQUIRE(Contains(plain_text, kBoxLightTopLeft));
        CHECK(Contains(plain_text, "qqbot/main 待审配对(2 条)"));
        CHECK(Contains(plain_text, "sender sender-a"));
        CHECK(Contains(plain_text, "剩 60 秒"));
        CHECK(Contains(plain_text, "(已过期)"));
        CHECK(Contains(plain_text, "/channel pairing approve <channel> <code>"));
    }
}

TEST_CASE("handler:用法/引导/失败反馈进 frame,不裸打印") {
    {
        // 无参子命令词:用法清单进键值对框("用法"做标题;首空格拆列后
        // key 与 value 之间是键值对分隔,不再连排)。
        const std::string out = RunChannel("bogus qqbot", dark);
        REQUIRE(Contains(out, kBoxLightTopLeft));
        const std::string text = StripAnsi(out);
        CHECK(Contains(text, "认不得"));
        CHECK(Contains(text, "用法"));
        CHECK(Contains(text, "/channel"));
        CHECK(Contains(text, "show <channel> [account]"));
        CHECK(Contains(text, "pairing approve|reject <channel> [account] <code>"));
    }
    {
        // 普通交互形态的 start:引导句进框。
        const std::string out = RunChannel("start qqbot", dark);
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), "普通交互形态"));
    }
    {
        // 空目标:用法框。
        const std::string out = RunChannel("show", dark);
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), "show <channel> [account]"));
    }
}

TEST_CASE("plain 主题:全族输出零转义字节、无框字形(T3/--no-color 路径)") {
    const auto channels = MakeChannels();
    const auto snapshot = MakeSnapshot(ChannelAccountState::Running);
    const std::vector<ChannelManager::AccountSnapshot> snapshots = {snapshot};
    const std::vector<ChannelManager::PendingPairingView> no_pending;
    for (const auto& lines : {FormatChannelsOverview(&channels, &snapshots, plain),
                              FormatChannelsOverview(nullptr, nullptr, plain),
                              FormatChannelShow(&snapshot, plain),
                              FormatChannelDoctor(&snapshot, plain),
                              FormatChannelPairingList("qqbot/main", &no_pending, 0, plain)}) {
        for (const std::string& line : lines) {
            CHECK(line.find("\x1b") == std::string::npos);
            CHECK(!Contains(line, kBoxLightTopLeft));
            CHECK(!Contains(line, kBoxLightVert));
        }
    }
    for (const std::string& args : {"bogus", "start qqbot", "show"}) {
        CAPTURE(args);
        const std::string out = RunChannel(args, plain);
        CHECK(out.find("\x1b") == std::string::npos);
        CHECK(!Contains(out, kBoxLightTopLeft));
        CHECK(!Contains(out, kBoxLightVert));
    }
    // 信息一字不少:表头与状态文本都在,只是没了色与框。gateway 引导句只在
    // 没挂运行态(snapshots=nullptr)时出现,单列一档验证。
    const auto overview = FormatChannelsOverview(&channels, &snapshots, plain);
    std::string joined;
    for (const std::string& line : overview) {
        joined += line + "\n";
    }
    CHECK(Contains(joined, "channel"));
    CHECK(Contains(joined, "running"));
    const auto no_runtime = FormatChannelsOverview(&channels, nullptr, plain);
    std::string no_runtime_text;
    for (const std::string& line : no_runtime) {
        no_runtime_text += line + "\n";
    }
    CHECK(Contains(no_runtime_text, "gateway not running"));
}
