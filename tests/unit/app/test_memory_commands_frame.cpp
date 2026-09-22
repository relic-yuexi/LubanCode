// TUI 排版批 1(tui优化todo.todo P1:/memory 全套)的输出形状册。
//   - 单子验收三条"实际会话内连敲"留真机,这里以测试承载:status 键值对
//     对齐、list 两层 bullet 标色与 "(全局记忆)" 行尾注、jobs 表格与
//     state 语义色、各子命令提示进 frame 不再裸打印;
//   - plain 主题(--no-color/T3 降级路径)钉零转义字节、无框字形——合同
//     第 3 条;
//   - /memory 无 --json/机器消费分支(源码核实),字节级不变一条天然满足。
//
// 走真 HandleMemoryCommand(真 ProjectMemory 指临时目录 + TermPort 改道
// 捕获),断言只看形状与相对位置,不看绝对宽度(测试进程探不到终端,
// MemoryFrameWidth()=0 按内容自适应;真机宽度统一由主人会话内连敲验收)。

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <sstream>
#include <string>

#include "app/commands/memory_commands.hpp"
#include "cli/i18n.hpp"
#include "cli/line_editor.hpp"  // DisplayWidthUtf8:对齐断言按显示列量
#include "cli/terminal_port.hpp"
#include "cli/theme.hpp"
#include "memory/project_memory.hpp"

using namespace lubancode;

namespace {

namespace fs = std::filesystem;

fs::path TempRoot(const std::string& name) {
    static int sequence = 0;
    static const std::string run_id = std::to_string(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const fs::path path = fs::temp_directory_path() /
                          ("lubancode-memory-frame-test-" + run_id + "-" + name + "-" +
                           std::to_string(++sequence));
    std::error_code ec;
    fs::remove_all(path, ec);
    fs::create_directories(path / "repo" / ".git", ec);  // 身份解析要认得出仓库
    return path;
}

// 捕获 TermOut(端口改道,RAII 收回)。
class OutputCapture {
public:
    OutputCapture() { cli::TermPort().Redirect(&stream_, nullptr); }
    ~OutputCapture() { cli::TermPort().Reset(); }
    std::string text() const { return stream_.str(); }

private:
    std::ostringstream stream_;
};

// 剥掉 CSI 序列(与 test_frame_helpers.cpp 同一把手写的尺)。
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

// 剥 ANSI 后按显示列断言用的坐标(中文/框线多字节,字节位对不上列位)。
int DisplayColOf(const std::string& row, const std::string& needle) {
    const std::size_t at = row.find(needle);
    if (at == std::string::npos) {
        return -1;
    }
    return static_cast<int>(cli::DisplayWidthUtf8(row.substr(0, at)));
}

// key 段之后 value 的起始显示列(key 补齐 + 两格列距后应处处同列)。
int ValueStartCol(const std::string& row, const std::string& key) {
    const std::size_t at = row.find(key);
    if (at == std::string::npos) {
        return -1;
    }
    std::size_t i = at + key.size();
    while (i < row.size() && row[i] == ' ') {
        ++i;
    }
    return static_cast<int>(cli::DisplayWidthUtf8(row.substr(0, i)));
}

constexpr const char* kBoxLightTopLeft = "\xe2\x94\x8c";  // ┌
constexpr const char* kBoxLightVert = "\xe2\x94\x82";     // │
constexpr const char* kBullet = "\xe2\x80\xa2";           // •

// 一只指向临时目录的 ProjectMemory + 命令上下文。成员声明序即构造序:
// root 先落盘,身份随 root 解析,options 在 store 之前配好(store 构造
// 时按值收走,事后改成员不再生效)。
struct MemoryRig {
    fs::path root;
    fs::path repo;
    memory::Options options;
    decltype(memory::ResolveProjectIdentity(fs::path{}, fs::path{})) identity;
    memory::ProjectMemory store;
    cli::Theme theme;
    app::MemoryCommandContext ctx;

    explicit MemoryRig(const std::string& name, const char* theme_name = "dark")
        : root(TempRoot(name)),
          repo(root / "repo"),
          options(MakeOptions()),
          identity(memory::ResolveProjectIdentity(repo, root / "home")),
          store(*identity, root / "home", options),
          theme(cli::BuiltinTheme(theme_name)) {
        REQUIRE(identity.has_value());
        ctx.project_memory = &store;
        ctx.theme = &theme;
    }

    std::string Run(const std::string& args) {
        OutputCapture capture;
        app::HandleMemoryCommand(ctx, args);
        return capture.text();
    }

private:
    static memory::Options MakeOptions() {
        memory::Options options;
        options.global_allowed = true;
        options.enabled = true;
        options.user_enabled = true;
        return options;
    }
};

memory::SaveRequest MakeSaveRequest() {
    memory::SaveRequest request;
    request.kind = memory::MemoryKind::Fact;
    request.title = "请求路径";
    request.summary = "AgentLoopRun 组装请求";
    request.content = "正文一句话。";
    return request;
}

}  // namespace

TEST_CASE("status: 键值对框,key 列全表对齐") {
    MemoryRig rig("status");
    const std::string out = rig.Run("status");

    // 框在(dark 主题画 Light 档),key 列上 row_label 色。
    REQUIRE(Contains(out, kBoxLightTopLeft));
    CHECK(Contains(out, rig.theme.row_label));

    // 拆句进两列:句内不再有 "key: value" 冒号连排,value 另起一列。
    const std::string plain = StripAnsi(out);
    CHECK(plain.find("全局授权: ") == std::string::npos);
    CHECK(Contains(plain, "全局授权"));

    // 对齐:两个不同 key 的行,value 起始显示列一致(key 补齐到最宽)。
    std::istringstream lines(plain);
    std::string line;
    int grant_value_col = -1;
    int workspace_value_col = -1;
    while (std::getline(lines, line)) {
        if (Contains(line, "全局授权")) grant_value_col = ValueStartCol(line, "全局授权");
        if (Contains(line, "工作区")) workspace_value_col = ValueStartCol(line, "工作区");
    }
    CHECK(grant_value_col > 0);
    CHECK(workspace_value_col > 0);
    CHECK(grant_value_col == workspace_value_col);
}

TEST_CASE("list: 两层条目一框,bullet 分档标色,user 层行尾带层注") {
    MemoryRig rig("list");
    memory::SaveRequest fact;
    fact.kind = memory::MemoryKind::Fact;
    fact.id = "fact.agent-loop.flow";
    fact.title = "请求路径";
    fact.summary = "AgentLoopRun 组装请求";
    fact.content = "正文一句话。";
    REQUIRE(rig.store.EnqueueSave(fact).has_value());
    memory::SaveRequest preference;
    preference.kind = memory::MemoryKind::Preference;
    preference.id = "preference.package-manager";
    preference.title = "包管理器";
    preference.summary = "用 yarn";
    preference.content = "用户要求用 yarn。";
    preference.scope.level = "user";
    preference.scope.kind = "user";
    REQUIRE(rig.store.EnqueueSave(preference, /*user_initiated=*/true,
                                  memory::MemoryWriteSource::ExplicitCommandSave)
                .has_value());
    REQUIRE(memory::RunPendingMemoryJobs(rig.root / "home").has_value());

    const std::string out = rig.Run("list");

    REQUIRE(Contains(out, kBoxLightTopLeft));
    // layer 标色两处落笔:bullet 两档主题色 + 行尾 "(全局记忆)"(key_hint)。
    CHECK(Contains(out, rig.theme.list_bullet_project));
    CHECK(Contains(out, rig.theme.list_bullet_user));
    CHECK(Contains(out, rig.theme.key_hint));
    const std::string plain = StripAnsi(out);
    CHECK(Contains(plain, cli::tr("cmd.memory.global_layer")));
    CHECK(Contains(plain, "preference.package-manager"));

    // id 列对齐:两层各一行,value("[" 起头)起始显示列同列。
    std::istringstream lines(plain);
    std::string line;
    int fact_col = -1;
    int preference_col = -1;
    while (std::getline(lines, line)) {
        if (Contains(line, "fact.agent-loop.flow")) fact_col = DisplayColOf(line, "[");
        if (Contains(line, "preference.package-manager")) preference_col = DisplayColOf(line, "[");
    }
    CHECK(fact_col > 0);
    CHECK(preference_col > 0);
    CHECK(fact_col == preference_col);
}

TEST_CASE("list: 空态带 frame 提示") {
    MemoryRig rig("list-empty");
    const std::string out = rig.Run("list");

    REQUIRE(Contains(out, kBoxLightTopLeft));
    CHECK(Contains(StripAnsi(out), cli::tr("cmd.memory.empty")));
}

TEST_CASE("jobs: 表格框 + state 语义色 + 附注框") {
    MemoryRig rig("jobs");
    memory::SaveRequest pending = MakeSaveRequest();
    pending.id = "fact.jobs.pending";
    pending.title = "待写条目";
    REQUIRE(rig.store.EnqueueSave(pending).has_value());  // 不跑 worker:留 pending

    const std::string out = rig.Run("jobs");

    // 主表:表头(字段名)+ 标题框。
    REQUIRE(Contains(out, kBoxLightTopLeft));
    const std::string plain = StripAnsi(out);
    CHECK(Contains(plain, cli::tr("cmd.memory.jobs.header")));
    bool saw_header_row = false;
    std::istringstream lines(plain);
    std::string line;
    while (std::getline(lines, line)) {
        if (Contains(line, "state") && Contains(line, "worker")) {
            saw_header_row = true;
        }
    }
    CHECK(saw_header_row);
    CHECK(Contains(plain, "pending"));
    // state 语义色:pending 走 table_skip 档(批 0 主题字段)。
    CHECK(Contains(out, rig.theme.table_skip + "pending" + rig.theme.reset));
    // 附注框:title_line 既有文案拆键值对进第二框。
    CHECK(Contains(plain, "标题"));
}

TEST_CASE("remember/排队: 成功提示进 frame,不再裸打印") {
    MemoryRig rig("remember");
    const std::string out = rig.Run("remember fact 构建入口 :: 走 cmake 预设");

    REQUIRE(Contains(out, kBoxLightTopLeft));
    const std::string plain = StripAnsi(out);
    CHECK(Contains(plain, "已排进后台队列"));
    // 拆句后 key 列不再带冒号连排。
    CHECK(plain.find("已排进后台队列: ") == std::string::npos);
}

TEST_CASE("on/use: 简短状态提示走键值对") {
    MemoryRig rig("toggle");
    {
        const std::string out = rig.Run("use off");
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), cli::tr("cmd.memory.retrieval")));
    }
    {
        const std::string out = rig.Run("on");
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), "本场已"));
    }
}

TEST_CASE("why: 无召回记录的提示也进框") {
    MemoryRig rig("why-none");
    const std::string out = rig.Run("why");
    REQUIRE(Contains(out, kBoxLightTopLeft));
    CHECK(Contains(StripAnsi(out), cli::tr("cmd.memory.why.none")));
}

TEST_CASE("show: 头部键值对框 + 正文原样跟出") {
    MemoryRig rig("show");
    memory::SaveRequest topic;
    topic.kind = memory::MemoryKind::Fact;
    topic.id = "fact.show.topic";
    topic.title = "展示主题";
    topic.content = "正文第一行。\n正文第二行。";
    REQUIRE(rig.store.EnqueueSave(topic).has_value());
    REQUIRE(memory::RunPendingMemoryJobs(rig.root / "home").has_value());

    const std::string out = rig.Run("show fact.show.topic");
    REQUIRE(Contains(out, kBoxLightTopLeft));
    // 头部框一行:id 在 key 列,目录走 muted 淡色。
    CHECK(Contains(out, "fact.show.topic"));
    CHECK(Contains(out, rig.theme.row_muted));
    // 正文不塞框(保终端折行),原样可读。
    CHECK(Contains(StripAnsi(out), "正文第二行。"));
}

TEST_CASE("plain 主题: 全套命令零转义字节、无框字形(T3/--no-color 路径)") {
    MemoryRig rig("plain", "plain");
    for (const std::string& args : {"status", "list", "jobs", "why", "stale", "review"}) {
        CAPTURE(args);
        const std::string out = rig.Run(args);
        CHECK(out.find("\x1b") == std::string::npos);
        CHECK(!Contains(out, kBoxLightTopLeft));
        CHECK(!Contains(out, kBoxLightVert));
        // 项目符退 "-",plain 不出圆点字形。
        CHECK(out.find(kBullet) == std::string::npos);
    }
    // 排队路也零转义(含 worker 启动失败的 error accent:plain 全空串)。
    const std::string queued = rig.Run("remember fact 纯文本 :: 没有颜色");
    CHECK(queued.find("\x1b") == std::string::npos);
    CHECK(!Contains(queued, kBoxLightTopLeft));
}
