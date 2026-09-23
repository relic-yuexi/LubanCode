// TUI 排版批 5a(tui优化todo.todo P5:/peer 全族)的输出形状册。
//   - /peers 非交互档(管道/spinner 关)走表格:peer/status/cwd 三列(批 5
//     单子"表格"档定案);交互菜单(ReadChoiceMenu)不是 render 段,原样;
//   - /send、/peerperm 的反馈句进无标题键值对框,失败/找不到目标走 error
//     档 accent;
//   - plain 主题(--no-color/T3 降级路径)钉零转义字节、无框字形——合同
//     第 3 条;
//   - /peer 无 --json/机器消费分支(源码核实),字节级不变一条天然满足。
//
// 走真 Handle*Command(off 档直调;表格分支起真 PeerRuntime + 临时名册目录
// 手写一枚他人名片——名册只认 <peer_id>.json,心跳新鲜即上榜)。断言只看
// 形状与相对位置,不看绝对宽度。

#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "app/commands/peer_commands.hpp"
#include "cli/line_editor.hpp"  // DisplayWidthUtf8:对齐断言按显示列量
#include "cli/terminal_port.hpp"
#include "cli/theme.hpp"
#include "peers/peer_session.hpp"

using namespace lubancode;
using namespace lubancode::app;

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

std::filesystem::path TempDir(const std::string& tag) {
    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path(ec) /
                     ("lubancode_peer_frame_" + tag + "_" + std::to_string(::rand()));
    std::filesystem::create_directories(dir, ec);
    return dir;
}

const cli::Theme dark = cli::BuiltinTheme("dark");
const cli::Theme plain = cli::BuiltinTheme("plain");

// 在名册目录里手写一枚他人名片(peer_id/endpoint 必填,心跳=当下,不写 pid
// ——pid=0 时新鲜度只看心跳,不必挂真进程)。
void WriteForeignCard(const std::filesystem::path& registry_dir, const std::string& peer_id,
                      const std::string& name, const std::string& status, const std::string& cwd) {
    const long long now = static_cast<long long>(::time(nullptr));
    std::ofstream out(registry_dir / (peer_id + ".json"), std::ios::binary);
    out << "{\"peer_id\":\"" << peer_id << "\",\"endpoint\":\"127.0.0.1:1\",\"name\":\"" << name
        << "\",\"cwd\":\"" << cwd << "\",\"status\":\"" << status << "\",\"pid\":0,\"started_at\":"
        << now << ",\"last_seen\":" << now << ",\"protocol_version\":1}";
}

std::string RunPeers(PeerCommandState& state, const cli::Theme& theme) {
    OutputCapture capture;
    HandlePeersCommand(state, theme, /*spinner_enabled=*/false);
    return capture.text();
}

}  // namespace

TEST_CASE("off 档:三命令反馈各进键值对框,off 句走 muted 档") {
    std::vector<peers::PeerEnvelope> ready;
    std::vector<peers::PeerEnvelope> held;
    std::optional<peers::PeerRuntime> idle;
    PeerCommandState off{idle, false, ready, held};

    const std::string peers_out = RunPeers(off, dark);
    REQUIRE(Contains(peers_out, kBoxLightTopLeft));
    CHECK(Contains(StripAnsi(peers_out), "跨会话传话在本场未启用"));
    CHECK(Contains(peers_out, dark.row_muted + "跨会话传话在本场未启用"));

    {
        OutputCapture capture;
        HandleSendCommand(off, "alpha 在吗", dark);
        const std::string out = capture.text();
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), "跨会话传话在本场未启用"));
    }
    {
        OutputCapture capture;
        HandlePeerpermCommand(off, "hold", dark);
        const std::string out = capture.text();
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), "跨会话传话在本场未启用"));
    }
}

TEST_CASE("名册有人:/peers 走表格,peer/status/cwd 三列对齐") {
    const auto dir = TempDir("roster");
    peers::PeerRuntimeOptions options;
    options.registry_dir = dir;
    options.name = "solo";
    options.cwd = dir.string();
    options.permission_mode = [] { return lubancode::ApprovalMode::Default; };
    WriteForeignCard(dir, "aabb01", "alpha", "idle", "D:/work/alpha");
    WriteForeignCard(dir, "ccdd02", "beta", "busy", "D:/work/beta-longer");

    std::optional<peers::PeerRuntime> runtime;
    runtime.emplace(std::move(options));
    std::string error;
    REQUIRE(runtime->Start(&error));
    std::vector<peers::PeerEnvelope> ready;
    std::vector<peers::PeerEnvelope> held;
    PeerCommandState on{runtime, true, ready, held};

    const std::string out = RunPeers(on, dark);
    const std::string text = StripAnsi(out);

    // 表格:表头三列 + 两行名片(alpha/beta 都上榜;自己的 solo 名片被滤)。
    REQUIRE(Contains(out, kBoxLightTopLeft));
    CHECK(Contains(text, "peer"));
    CHECK(Contains(text, "status"));
    CHECK(Contains(text, "cwd"));
    CHECK(Contains(text, "alpha (aabb01)"));
    CHECK(Contains(text, "beta (ccdd02)"));
    CHECK(Contains(text, "空闲"));
    CHECK(Contains(text, "忙"));
    CHECK(!Contains(text, "solo"));

    // 对齐:两行的 status 单元格同列(peer 列宽按最宽行对齐)。
    int alpha_col = -1;
    int beta_col = -1;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (Contains(line, "alpha")) alpha_col = DisplayColOf(line, "空闲");
        if (Contains(line, "beta")) beta_col = DisplayColOf(line, "忙");
    }
    CHECK(alpha_col > 0);
    CHECK(beta_col > 0);
    CHECK(alpha_col == beta_col);

    // /send 找不到人:error 档键值对框。
    {
        OutputCapture capture;
        HandleSendCommand(on, "who-is-this 你好", dark);
        const std::string send_out = capture.text();
        REQUIRE(Contains(send_out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(send_out), "找不到会话"));
        CHECK(Contains(send_out, dark.error));
    }
    // /send 缺参数:用法句进框(句内冒号拆列)。
    {
        OutputCapture capture;
        HandleSendCommand(on, "no-space", dark);
        const std::string send_out = capture.text();
        REQUIRE(Contains(send_out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(send_out), "用法"));
    }

    runtime->Stop();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("peerperm:看/设/用法反馈各进框,键列走 row_label") {
    const auto dir = TempDir("perm");
    peers::PeerRuntimeOptions options;
    options.registry_dir = dir;
    options.name = "solo";
    options.cwd = dir.string();
    options.permission_mode = [] { return lubancode::ApprovalMode::Default; };
    std::optional<peers::PeerRuntime> runtime;
    runtime.emplace(std::move(options));
    std::string error;
    REQUIRE(runtime->Start(&error));
    std::vector<peers::PeerEnvelope> ready;
    std::vector<peers::PeerEnvelope> held;
    PeerCommandState on{runtime, true, ready, held};

    {
        OutputCapture capture;
        HandlePeerpermCommand(on, "", dark);
        const std::string out = capture.text();
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), "当前跨会话来信档"));
        CHECK(Contains(out, dark.row_label + "当前跨会话来信档"));
    }
    {
        OutputCapture capture;
        HandlePeerpermCommand(on, "hold", dark);
        const std::string out = capture.text();
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), "已设为 hold"));
    }
    {
        OutputCapture capture;
        HandlePeerpermCommand(on, "nonsense", dark);
        const std::string out = capture.text();
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), "用法"));
    }

    runtime->Stop();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("plain 主题:全族输出零转义字节、无框字形(T3/--no-color 路径)") {
    std::vector<peers::PeerEnvelope> ready;
    std::vector<peers::PeerEnvelope> held;
    std::optional<peers::PeerRuntime> idle;
    PeerCommandState off{idle, false, ready, held};

    const std::string peers_out = RunPeers(off, plain);
    CHECK(peers_out.find("\x1b") == std::string::npos);
    CHECK(!Contains(peers_out, kBoxLightTopLeft));
    CHECK(!Contains(peers_out, kBoxLightVert));
    CHECK(Contains(peers_out, "跨会话传话在本场未启用"));  // 信息一字不少

    {
        OutputCapture capture;
        HandleSendCommand(off, "alpha 在吗", plain);
        const std::string out = capture.text();
        CHECK(out.find("\x1b") == std::string::npos);
        CHECK(!Contains(out, kBoxLightTopLeft));
    }
    {
        OutputCapture capture;
        HandlePeerpermCommand(off, "hold", plain);
        const std::string out = capture.text();
        CHECK(out.find("\x1b") == std::string::npos);
        CHECK(!Contains(out, kBoxLightTopLeft));
    }
}
