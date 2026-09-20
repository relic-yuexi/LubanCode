// 按代理状态投影单 P0/P1 的主回归册:混屏通路的复现与投影的分账。
//
// 测试替身是一块"虚拟屏"(ConsoleTestHooks + TerminalPort 改道):既有
// 写屏栈(StreamBodyTracker 的锚点账、ToolDisplay/TranscriptPainter 的
// 原地改写、帧账原语)原封不动地跑在上面——写屏的坐标、次序、末帧内容
// 全部可断言,不再只看最终字符串或 selected id(单子 §八的测试分三层,
// 这册是第二层"含可控交错屏障的帧/事件调度测试")。
//
// 钉的桩:
//   1. P0 取证:无投影旧路(registry 空)下,main 流式途中"切 sub"后
//      main 的字照写屏——观察器(ui_trace + 虚拟屏)抓得住,这是证据
//      基建的有效性证明(修复前的混屏形态)。
//   2. P1 投影:同一交错序列在登记簿挂上后,离屏期间零写屏、零 commit,
//      而视图账/工具配对/usage 一分不少;切回 main 成对重铺,正文恰好一
//      份、工具恰好一张;水位之后的增量接续活画。
//   3. 成对协议:重铺事务持泵画笔锁——重铺期间投递的事件不丢不重
//      (事务后接续,不越过水位)。

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/types.hpp"
#include "app/agent_view_registry.hpp"
#include "app/session_ui_dispatcher.hpp"
#include "app/terminal_turn_sink.hpp"
#include "cli/agent_view_state.hpp"
#include "cli/approval_channel.hpp"  // P3:换代把悬着的审批按拒收口
#include "cli/console_input.hpp"  // StdoutWriteMutex:换页打印的锁纪律与生产一致
#include "cli/terminal_port.hpp"
#include "cli/theme.hpp"
#include "cli/tool_display.hpp"
#include "cli/turn_renderer.hpp"
#include "cli/ui_trace.hpp"
#include "platform/console.hpp"
#include "runtime/id_authority.hpp"
#include "runtime/turn_collector.hpp"
#include "runtime/turn_runtime.hpp"
#include "runtime/turn_view.hpp"
#include "tools/tool.hpp"

using namespace lubancode;

namespace {

// ---------------------------------------------------------------------------
// 虚拟屏:Windows 长缓冲语义(绝对行号 + 高缓冲 + 可平移视口),字节流
// 进自定义 streambuf,ANSI 剥离落格;CUP/EL 两枚定位序列解析成坐标操作
// (VT 批路径万一被缓存成开,画面照样对)。
// ---------------------------------------------------------------------------
class VirtualScreen {
public:
    static constexpr int kWidth = 80;
    static constexpr int kHeight = 9000;   // 长缓冲:测试量级的正文滚不到底
    static constexpr int kViewport = 24;

    std::vector<std::string> rows;  // 每行的可见字符(宽字算 1 格,测试数据用 ASCII)
    int cursor_x = 0;
    int cursor_y = 0;
    int viewport_y = 0;
    std::mutex mutex;  // 泵消费线程与测试线程都可能落笔

    std::string Raw() {
        std::lock_guard<std::mutex> lock(mutex);
        return raw_.str();
    }

    void Reset() {
        std::lock_guard<std::mutex> lock(mutex);
        DoResetLocked();
    }

    // 可见文本(整屏拼接;断言"谁在屏上"用)。
    std::string VisibleText() {
        std::lock_guard<std::mutex> lock(mutex);
        std::string joined;
        for (const std::string& row : rows) {
            joined += row;
            joined += '\n';
        }
        return joined;
    }

    std::optional<std::string> RowText(int row) {
        std::lock_guard<std::mutex> lock(mutex);
        if (row < 0 || row >= static_cast<int>(rows.size())) {
            return std::nullopt;
        }
        // 行尾空格剥掉(ReadRowText 同款)。
        std::string text = rows[static_cast<std::size_t>(row)];
        while (!text.empty() && text.back() == ' ') {
            text.pop_back();
        }
        return text;
    }

    // 视口顶的绝对行号(pan_viewport_down 平移后 >0)。断言"帧在视口顶"
    // 用它换算,不假设视口恒在 0 行——长缓冲下"保锚可见"原语会把视口
    // 跟着内容往下挪,那是生产语义,不是串页。
    int ViewportTop() {
        std::lock_guard<std::mutex> lock(mutex);
        return viewport_y;
    }

    // 安装(构造即装,析构即拆):ConsoleTestHooks + TermPort 双改道。
    explicit VirtualScreen() {
        Install();
    }
    ~VirtualScreen() {
        Uninstall();
    }
    VirtualScreen(const VirtualScreen&) = delete;
    VirtualScreen& operator=(const VirtualScreen&) = delete;

private:
    void DoResetLocked() {
        rows.assign(1, std::string());
        cursor_x = 0;
        cursor_y = 0;
        viewport_y = 0;
        raw_.str(std::string());
        ansi_.clear();
        in_ansi_ = false;
    }

    // ---- 字节落格(剥 ANSI;CUP/EL 解析) ----
    void Feed(char c) {
        if (in_ansi_) {
            ansi_ += c;
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
                ApplyAnsi(ansi_);
                ansi_.clear();
                in_ansi_ = false;
            }
            return;
        }
        if (c == '\x1b') {
            in_ansi_ = true;
            ansi_ = std::string(1, c);
            return;
        }
        raw_ << c;
        if (c == '\n') {
            cursor_x = 0;
            ScrollRow();
            ++cursor_y;
            return;
        }
        if (c == '\r') {
            cursor_x = 0;
            return;
        }
        if (c == '\t') {
            cursor_x = (cursor_x / 8 + 1) * 8;
            return;
        }
        if (cursor_x >= kWidth) {  // 自动折行(真控制台行为)
            cursor_x = 0;
            ScrollRow();
            ++cursor_y;
        }
        Place(cursor_x, cursor_y, c);
        ++cursor_x;
    }

    void ScrollRow() {
        if (cursor_y + 1 >= kHeight) {
            // 贴缓冲底:内容整体上滚一行(测试量级到不了,防御性实现)。
            rows.erase(rows.begin());
            rows.emplace_back();
            --cursor_y;
        }
    }

    void Place(int x, int y, char c) {
        if (y < 0) {
            return;
        }
        if (static_cast<int>(rows.size()) <= y) {
            rows.resize(static_cast<std::size_t>(y) + 1, std::string());
        }
        std::string& row = rows[static_cast<std::size_t>(y)];
        if (static_cast<int>(row.size()) <= x) {
            row.resize(static_cast<std::size_t>(x) + 1, ' ');
        }
        row[static_cast<std::size_t>(x)] = c;
    }

    void ClearRange(int x, int y, int count) {
        if (y < 0 || y >= static_cast<int>(rows.size())) {
            return;
        }
        std::string& row = rows[static_cast<std::size_t>(y)];
        for (int i = 0; i < count; ++i) {
            const int at = x + i;
            if (at >= kWidth) {
                break;
            }
            if (at >= static_cast<int>(row.size())) {
                break;
            }
            row[static_cast<std::size_t>(at)] = ' ';
        }
    }

    void ApplyAnsi(const std::string& sequence) {
        // 只解析影响坐标/画面的:CUP(H)、EL(K);SGR(m) 与私有序列忽略。
        if (sequence.size() < 2 || sequence[0] != '\x1b' || sequence[1] != '[') {
            return;
        }
        const std::string body = sequence.substr(2, sequence.size() - 3);
        const char command = sequence.back();
        const auto split = [&body]() -> std::pair<int, int> {
            const std::size_t semi = body.find(';');
            const int first = semi == std::string::npos ? 1 : std::atoi(body.c_str());
            const int second = semi == std::string::npos ? 1 : std::atoi(body.c_str() + semi + 1);
            return {first == 0 ? 1 : first, second == 0 ? 1 : second};
        };
        switch (command) {
            case 'H':
            case 'f': {
                const auto [row, col] = split();
                cursor_y = row - 1;
                cursor_x = col - 1;
                break;
            }
            case 'A':
                cursor_y -= std::max(1, std::atoi(body.c_str()));
                break;
            case 'B':
                cursor_y += std::max(1, std::atoi(body.c_str()));
                break;
            case 'C':
                cursor_x += std::max(1, std::atoi(body.c_str()));
                break;
            case 'D':
                cursor_x -= std::max(1, std::atoi(body.c_str()));
                break;
            case 'K': {
                const int mode = body.empty() ? 0 : std::atoi(body.c_str());
                if (mode == 0) {
                    if (static_cast<int>(rows.size()) > cursor_y) {
                        std::string& row = rows[static_cast<std::size_t>(cursor_y)];
                        if (static_cast<int>(row.size()) > cursor_x) {
                            row.resize(static_cast<std::size_t>(cursor_x));
                        }
                    }
                } else if (mode == 2) {
                    if (static_cast<int>(rows.size()) > cursor_y) {
                        rows[static_cast<std::size_t>(cursor_y)].clear();
                    }
                }
                break;
            }
            default:
                break;  // SGR(m) 等与画面坐标无关
        }
    }

    // ---- 安装/拆卸 ----
    void Install() {
        DoResetLocked();
        buffer_ = std::make_unique<ScreenBuf>(this);
        cli::TermPort().Redirect(&buffer_->stream(), &buffer_->stream());
        platform::ConsoleTestHooks hooks;
        hooks.get_screen_info = [this]() -> std::optional<platform::ScreenInfo> {
            std::lock_guard<std::mutex> lock(mutex);
            platform::ScreenInfo info;
            info.width = kWidth;
            info.height = kHeight;
            info.cursor_x = cursor_x;
            info.cursor_y = cursor_y;
            info.viewport_x = 0;
            info.viewport_y = viewport_y;
            info.viewport_height = kViewport;
            return info;
        };
        hooks.set_cursor_pos = [this](int x, int y) {
            std::lock_guard<std::mutex> lock(mutex);
            cursor_x = std::max(0, x);
            cursor_y = std::max(0, y);
        };
        hooks.clear_row_from = [this](int x, int y, int count) {
            std::lock_guard<std::mutex> lock(mutex);
            ClearRange(x, y, count);
        };
        hooks.clear_row_hard_from = [this](int x, int y, int count) {
            std::lock_guard<std::mutex> lock(mutex);
            ClearRange(x, y, count);
        };
        hooks.pan_viewport_down = [this](int rows_to_pan) -> int {
            std::lock_guard<std::mutex> lock(mutex);
            const int room = kHeight - (viewport_y + kViewport);
            const int actual = std::min(rows_to_pan, room);
            if (actual > 0) {
                viewport_y += actual;
            }
            return actual;
        };
        hooks.console_width = []() -> std::optional<int> { return kWidth; };
        hooks.read_row_text = [this](int row) -> std::optional<std::string> { return RowText(row); };
        hooks.override_screen_repaint = true;
        hooks.wants_screen_repaint = true;
        platform::SetConsoleTestHooks(std::move(hooks));
    }

    void Uninstall() {
        platform::SetConsoleTestHooks(platform::ConsoleTestHooks{});
        cli::TermPort().Reset();
    }

    // streambuf:字节流进虚拟屏。
    class ScreenBuf : public std::streambuf {
    public:
        explicit ScreenBuf(VirtualScreen* screen) : screen_(screen) {
            setp(nullptr, nullptr);
            setg(nullptr, nullptr, nullptr);
        }
        std::ostream& stream() {
            if (!owned_stream_) {
                owned_stream_ = std::make_unique<std::ostream>(this);
            }
            return *owned_stream_;
        }

    protected:
        int overflow(int ch) override {
            if (ch != traits_type::eof()) {
                const char c = static_cast<char>(ch);
                std::lock_guard<std::mutex> lock(screen_->mutex);
                screen_->Feed(c);
            }
            return ch;
        }
        std::streamsize xsputn(const char* s, std::streamsize count) override {
            std::lock_guard<std::mutex> lock(screen_->mutex);
            for (std::streamsize i = 0; i < count; ++i) {
                screen_->Feed(s[i]);
            }
            return count;
        }

    private:
        VirtualScreen* screen_;
        std::unique_ptr<std::ostream> owned_stream_;
    };

    friend class ScreenBuf;

    std::ostringstream raw_;
    std::string ansi_;
    bool in_ansi_ = false;
    std::unique_ptr<ScreenBuf> buffer_;
};

// ---------------------------------------------------------------------------
// 事件工厂(sink 吃什么,这里造什么——形状对齐 TurnEventAdapter)。
// ---------------------------------------------------------------------------
runtime::ServerEvent MakeDelta(const std::string& item_id, const std::string& text,
                               runtime::ItemKind kind = runtime::ItemKind::Text) {
    runtime::ServerEvent event;
    event.kind = runtime::ServerEventKind::ItemDelta;
    event.item_id = item_id;
    event.item_kind = kind;
    event.text = text;
    event.envelope.seq = 0;
    return event;
}

runtime::ServerEvent MakeToolStart(const std::string& item_id, const std::string& tool_use_id,
                                    const std::string& name) {
    runtime::ServerEvent event;
    event.kind = runtime::ServerEventKind::ItemStarted;
    event.item_id = item_id;
    event.item_kind = runtime::ItemKind::Tool;
    event.payload["tool_use_id"] = tool_use_id;
    event.payload["tool_name"] = name;
    event.payload["input"] = nlohmann::json::object({{"path", "demo.txt"}});
    return event;
}

runtime::ServerEvent MakeToolDone(const std::string& item_id, const std::string& tool_use_id,
                                   const std::string& name, const std::string& result) {
    runtime::ServerEvent event;
    event.kind = runtime::ServerEventKind::ItemCompleted;
    event.item_id = item_id;
    event.item_kind = runtime::ItemKind::Tool;
    event.payload["tool_use_id"] = tool_use_id;
    event.payload["result"] = result;
    event.payload["is_error"] = false;
    (void)name;
    return event;
}

runtime::ServerEvent MakeUsage(std::int64_t input_tokens = 120, std::int64_t output_tokens = 45) {
    runtime::ServerEvent event;
    event.kind = runtime::ServerEventKind::UsageUpdated;
    event.payload["input_tokens"] = input_tokens;
    event.payload["output_tokens"] = output_tokens;
    event.payload["reported_by_provider"] = true;
    event.payload["reported"] = true;
    return event;
}

// 一套"活回合"的装配:sink + collector + display + tracker,与 RunTurn 同
// 款 Ingredients(registry 可空 = 旧路)。theme 是成员且声明在前——
// display/body 只存引用,先于它们析构就是悬垂。
struct LiveTurnHarness {
    const cli::Theme theme{cli::BuiltinTheme("dark")};
    std::vector<cli::TranscriptItem> transcript;
    std::atomic<bool> cancel_flag{false};
    std::atomic<bool> expanded{false};
    std::unique_ptr<cli::ToolDisplay> display;
    std::unique_ptr<cli::StreamBodyTracker> body;
    std::unique_ptr<runtime::TurnCollector> collector;
    runtime::TurnUsageStats usage;
    std::unique_ptr<app::TerminalTurnSink> sink;

    explicit LiveTurnHarness(app::AgentViewRegistry* registry_to_wire,
                             app::SessionUiDispatcher* dispatcher_to_wire = nullptr) {
        display = std::make_unique<cli::ToolDisplay>(transcript, theme, /*console=*/true, nullptr,
                                                     &cancel_flag, &expanded, /*silent=*/false);
        body = std::make_unique<cli::StreamBodyTracker>(theme, /*enabled=*/true, /*silent=*/false);
        collector = std::make_unique<runtime::TurnCollector>(runtime::ProcessIdAuthority(), "turn-projection");
        collector->StartTurn("用户的问题", 1000);
        app::TerminalTurnSink::Ingredients ingredients;
        ingredients.display = display.get();
        ingredients.body_tracker = body.get();
        ingredients.view_collector = collector.get();
        ingredients.usage_stats = &usage;
        ingredients.cancel_flag = &cancel_flag;
        ingredients.view_registry = registry_to_wire;
        ingredients.ui_dispatcher = dispatcher_to_wire;
        sink = std::make_unique<app::TerminalTurnSink>(std::move(ingredients));
    }
};

// P2 调度版装配:sink 的事件提交会话级调度,产生事件的线程不碰终端。
// 成员序:dispatcher 先声明(后析构),turn 的 sink 析构(DetachRenderer)
// 发生在 dispatcher 还活着的时候。
struct DispatchedHarness {
    app::SessionUiDispatcher dispatcher;
    LiveTurnHarness turn;
    explicit DispatchedHarness(app::AgentViewRegistry* registry_to_wire)
        : turn(registry_to_wire, &dispatcher) {}
};

// 等修订号到位(泵消费线程异步,delta 走投递路)。
bool WaitRevision(app::AgentViewRegistry& registry, std::uint64_t target,
                  std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (registry.MainRevision() >= target) {
            // 再让一拍:revision 到位不代表 DrawEvent 已跑完,末帧要稳。
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

// 等台账行数到位(无登记簿的旧路没有修订号,拿 ui_trace 的行数当水位;
// size_fn 自带锁,读侧安全)。
bool WaitTraceLines(std::size_t target, const std::function<std::size_t()>& size_fn,
                    std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (size_fn() >= target) {
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

// 模拟 print_view_frame 的擦屏半边(ClearVisibleAgentPanelLocked 同款):
// 可视区整块清净、光标回视口顶。须在 StdoutWriteMutex 内调。
void ClearViewportForViewSwitch() {
    const std::optional<platform::ScreenInfo> info = platform::GetScreenInfo();
    if (!info.has_value()) {
        return;
    }
    const int viewport_height =
        info->viewport_height > 0 ? info->viewport_height : info->height;
    const int top = info->viewport_y;
    for (int y = top; y < top + viewport_height && y < info->height; ++y) {
        platform::ClearRowHardFrom(0, y, info->width);
    }
    platform::SetCursorPos(0, top);
}

// 打印一帧 main 活回合(与 PrintViewedTranscript 的活回合路同一颗
// renderer),须在 StdoutWriteMutex 内调。
void PrintMainFrameLines(const runtime::TurnView& view) {
    cli::TurnRenderOptions options;
    options.width = VirtualScreen::kWidth;
    options.plain = false;
    options.include_text = true;
    const std::vector<std::string> lines = cli::RenderTurnView(view, cli::BuiltinTheme("dark"), options);
    for (const std::string& line : lines) {
        cli::TermOut() << line << "\n";
    }
    cli::TermOut().flush();
}

}  // namespace

// ---------------------------------------------------------------------------
// P0 取证:无投影旧路,main 流式中途"切 sub"后照写屏——观察器抓得住。
// 这册证明证据基建(虚拟屏 + ui_trace 三关卡)能把混屏摆到台面上;
// 投影(下一册)合上后,同样的交错序列不再有这些字。
// ---------------------------------------------------------------------------
TEST_CASE("P0 取证: 无投影旧路——切看 sub 后 main 的字仍进屏,三关卡账目齐全") {
    VirtualScreen screen;
    // 台账录音机:泵消费线程写、测试线程读,加小锁。
    struct TraceLog {
        std::mutex mutex;
        std::vector<cli::ui_trace::Record> records;
    } log;
    cli::ui_trace::InstallSink([&](const cli::ui_trace::Record& record) {
        std::lock_guard<std::mutex> lock(log.mutex);
        log.records.push_back(record);
    });
    const auto trace_size = [&] {
        std::lock_guard<std::mutex> lock(log.mutex);
        return log.records.size();
    };

    LiveTurnHarness harness(nullptr);  // registry 空 = 旧路(投影未挂)
    harness.sink->Emit(MakeDelta("item-text", "first paragraph before switch.\n"));
    REQUIRE(WaitTraceLines(2, trace_size));  // received + committed 各一
    // "切 sub":旧路没有登记簿,这里只切一个名义值——画面闸不存在。
    harness.sink->Emit(MakeDelta("item-text", "MAIN LEAK SENTENCE while viewing sub.\n"));
    REQUIRE(WaitTraceLines(4, trace_size));
    harness.sink->Emit(MakeToolStart("item-tool", "toolu_1", "read_file"));
    harness.sink->Emit(MakeToolDone("item-tool", "toolu_1", "read_file", "file body"));
    harness.sink->Emit(MakeUsage());
    REQUIRE(WaitTraceLines(10, trace_size));
    harness.sink->StopUiPump();

    // 混屏事实:main 的字与工具卡都进了屏(旧路没有按页投影)。
    const std::string visible = screen.VisibleText();
    CHECK(visible.find("first paragraph") != std::string::npos);
    CHECK(visible.find("MAIN LEAK SENTENCE") != std::string::npos);
    CHECK(visible.find("read_file") != std::string::npos);

    // 三关卡账目:接收/提交都记了,owner=main(无登记簿时 gen=0)。
    std::size_t received = 0;
    std::size_t committed = 0;
    {
        std::lock_guard<std::mutex> lock(log.mutex);
        for (const auto& record : log.records) {
            if (record.stage == cli::ui_trace::Stage::Received) {
                ++received;
                CHECK(record.owner.task_id == 0);
            } else if (record.stage == cli::ui_trace::Stage::Committed) {
                ++committed;
                CHECK(record.writer == "sink");
            }
        }
    }
    CHECK(received == 5);
    CHECK(committed == 5);  // 旧路:每笔都画了

    cli::ui_trace::InstallSink(nullptr);
}

// ---------------------------------------------------------------------------
// P1 投影:同一交错序列,登记簿挂上后——离屏零写屏、账一分不少、切回
// 成对重铺、水位之后接续活画。
// ---------------------------------------------------------------------------
TEST_CASE("P1 投影: main 流式途中切 sub——离屏零 commit,账照走,切回按水位接续") {
    VirtualScreen screen;
    app::AgentViewRegistry registry;  // 独立登记簿(测试内新建,不碰会话单例)
    {
        LiveTurnHarness harness(&registry);
        registry.BeginMainTurn(harness.collector.get(), &harness.sink->CommitMutex());

        // main 在屏:第一段正文活画。
        harness.sink->Emit(MakeDelta("item-text", "first paragraph before switch.\n"));
        REQUIRE(WaitRevision(registry, 1));
        // 切 sub(模拟监听线程的换页事务:画笔锁内清可视区、切身份、铺
        // sub 帧头——与 print_view_frame 的擦/铺半边同款)。
        registry.WithMainRenderLock([&] {
            {
                std::lock_guard<std::mutex> stdout_lock(cli::StdoutWriteMutex());
                ClearViewportForViewSwitch();
                cli::TermOut() << "== sub agent #7 view frame ==\n";
                cli::TermOut().flush();
            }
            registry.SwitchViewed(7);
        });
        // main 继续思考/出工具结果/记账——这正是截图混屏通路。
        harness.sink->Emit(MakeDelta("item-thinking", "hidden thinking", runtime::ItemKind::Thinking));
        harness.sink->Emit(MakeDelta("item-text", "MAIN LEAK SENTENCE while viewing sub.\n"));
        harness.sink->Emit(MakeToolStart("item-tool", "toolu_1", "read_file"));
        harness.sink->Emit(MakeToolDone("item-tool", "toolu_1", "read_file", "file body result"));
        harness.sink->Emit(MakeUsage());
        REQUIRE(WaitRevision(registry, 6));

        // 离屏断言:sub 帧在屏,main 的后续内容一个字都没进来。
        const std::string visible = screen.VisibleText();
        CHECK(visible.find("sub agent #7 view frame") != std::string::npos);
        CHECK(visible.find("first paragraph") == std::string::npos);  // 旧 main 帧已被擦
        CHECK(visible.find("MAIN LEAK SENTENCE") == std::string::npos);
        CHECK(visible.find("hidden thinking") == std::string::npos);
        CHECK(visible.find("read_file") == std::string::npos);

        // 账一分不少:视图账里有离屏期间的正文与工具配对,usage 记了。
        const app::AgentViewRegistry::MainTurnSnapshot ledger = registry.MainLedgeSnapshot();
        REQUIRE(ledger.view != nullptr);
        std::string joined_items;
        int tool_items = 0;
        for (const auto& item : ledger.view->items) {
            joined_items += item.result_text;
            if (item.kind == runtime::TurnItemViewKind::Tool) {
                ++tool_items;
            }
        }
        CHECK(joined_items.find("MAIN LEAK SENTENCE") != std::string::npos);
        CHECK(joined_items.find("hidden thinking") != std::string::npos);
        REQUIRE(tool_items == 1);
        CHECK(ledger.view->items.back().tool_name == "read_file");
        CHECK(harness.usage.request_count() == 1);
        CHECK(harness.usage.total_input_tokens() == 120);

        // 切回 main:成对重铺(画笔锁内清屏、切身份、取快照 → 打印 → 钉水位)。
        registry.WithMainRenderLock([&] {
            registry.SwitchViewed(0);
            const app::AgentViewRegistry::MainTurnSnapshot snapshot = registry.TakeMainLedgeForRepaint();
            {
                std::lock_guard<std::mutex> stdout_lock(cli::StdoutWriteMutex());
                ClearViewportForViewSwitch();
                PrintMainFrameLines(*snapshot.view);
            }
            registry.MarkMainPrinted(snapshot.revision);
        });

        // 重铺断言:正文恰好一份(不漏不重)、工具恰好一张。
        const std::string after = screen.VisibleText();
        CHECK(after.find("first paragraph") != std::string::npos);
        CHECK(after.find("MAIN LEAK SENTENCE") != std::string::npos);
        CHECK(after.find("read_file") != std::string::npos);
        CHECK(std::count(after.begin(), after.end(), 'M') >= 1);
        // 恰好一份:统计子串出现次数。
        const auto occurrences = [](const std::string& haystack, const std::string& needle) {
            std::size_t count = 0;
            for (std::size_t at = haystack.find(needle); at != std::string::npos;
                 at = haystack.find(needle, at + needle.size())) {
                ++count;
            }
            return count;
        };
        CHECK(occurrences(after, "MAIN LEAK SENTENCE") == 1);
        CHECK(occurrences(after, "first paragraph") == 1);
        CHECK(occurrences(after, "read_file") == 1);

        // 水位之后:新 delta 接续活画(闸门重开)。
        harness.sink->Emit(MakeDelta("item-text", "tail paragraph after return.\n"));
        REQUIRE(WaitRevision(registry, 7));
        harness.sink->StopUiPump();  // 排干余量,末帧定案
        const std::string tail = screen.VisibleText();
        CHECK(tail.find("tail paragraph after return") != std::string::npos);
        CHECK(occurrences(tail, "first paragraph") == 1);  // 没被再铺一遍

        registry.DetachMainTurn();
    }
}

// ---------------------------------------------------------------------------
// P1 成对协议:重铺事务期间到达的事件——照常收账,绘制让路;事务收拍后
// 按新水位接续,不越过水位重放。
// ---------------------------------------------------------------------------
TEST_CASE("P1 成对协议: 重铺事务持画笔锁——事务内事件不重放、不漏字") {
    VirtualScreen screen;
    app::AgentViewRegistry registry;
    LiveTurnHarness harness(&registry);
    registry.BeginMainTurn(harness.collector.get(), &harness.sink->CommitMutex());

    harness.sink->Emit(MakeDelta("item-text", "before transaction.\n"));
    REQUIRE(WaitRevision(registry, 1));

    // 模拟换页钩子在另一线程请求重铺,与泵消费线程并发:事务先拿画笔锁,
    // 期间泵里的 delta 照投(PostDelta 不阻塞)、收账等锁。
    std::atomic<bool> transaction_done{false};
    std::thread listener([&] {
        registry.WithMainRenderLock([&] {
            const app::AgentViewRegistry::MainTurnSnapshot snapshot = registry.TakeMainLedgeForRepaint();
            // 事务打印窗口:此刻另一线程投递增量(模拟 SSE 在飞)。
            std::thread producer([&] {
                harness.sink->Emit(MakeDelta("item-text", "during transaction.\n"));
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            cli::TurnRenderOptions options;
            options.width = VirtualScreen::kWidth;
            options.include_text = true;
            const std::vector<std::string> lines = cli::RenderTurnView(*snapshot.view,
                                                                       cli::BuiltinTheme("dark"), options);
            {
                std::lock_guard<std::mutex> stdout_lock(cli::StdoutWriteMutex());
                for (const std::string& line : lines) {
                    cli::TermOut() << line << "\n";
                }
            }
            registry.MarkMainPrinted(snapshot.revision);
            producer.join();
        });
        transaction_done.store(true);
    });
    // 等事务收拍、增量收账到位。
    REQUIRE(WaitRevision(registry, 2));
    while (!transaction_done.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    listener.join();
    harness.sink->StopUiPump();

    // "during transaction" 在快照之后收账,不在重铺帧里;水位放行后由泵
    // 活画(此刻 main 是当前页)——两处恰好合计一份。
    const std::string visible = screen.VisibleText();
    const auto occurrences = [](const std::string& haystack, const std::string& needle) {
        std::size_t count = 0;
        for (std::size_t at = haystack.find(needle); at != std::string::npos;
             at = haystack.find(needle, at + needle.size())) {
            ++count;
        }
        return count;
    };
    CHECK(occurrences(visible, "during transaction") <= 1);   // 不重放
    CHECK(registry.MainLedgeSnapshot().view != nullptr);
    std::string joined;
    for (const auto& item : registry.MainLedgeSnapshot().view->items) {
        joined += item.result_text;
    }
    CHECK(joined.find("during transaction") != std::string::npos);  // 不漏账
    registry.DetachMainTurn();
}

// ---------------------------------------------------------------------------
// 收口:EndMainTurn 后切回 main 重铺吃终账(chrome 让路的回合,字都在)。
// ---------------------------------------------------------------------------
TEST_CASE("P1 收口账: 回合在离屏期收口——终账在 ledge,切回重铺全文可见") {
    VirtualScreen screen;
    app::AgentViewRegistry registry;
    LiveTurnHarness harness(&registry);
    registry.BeginMainTurn(harness.collector.get(), &harness.sink->CommitMutex());

    registry.WithMainRenderLock([&] { registry.SwitchViewed(3); });
    harness.sink->Emit(MakeDelta("item-text", "offline final answer.\n"));
    REQUIRE(WaitRevision(registry, 1));
    harness.sink->StopUiPump();
    // RunTurn 的收口漏斗:FinishTurn 进锁、终账钉进 ledge。
    registry.ApplyToMainTurn([&] { harness.collector->FinishTurn(runtime::TurnItemViewState::Succeeded, 500, 0); });
    registry.EndMainTurn(harness.collector->view());

    // 屏上此刻没有 main 的字(全程离屏)。
    CHECK(screen.VisibleText().find("offline final answer") == std::string::npos);

    // 切回重铺:终账全文可见。
    registry.WithMainRenderLock([&] {
        registry.SwitchViewed(0);
        const app::AgentViewRegistry::MainTurnSnapshot snapshot = registry.TakeMainLedgeForRepaint();
        cli::TurnRenderOptions options;
        options.width = VirtualScreen::kWidth;
        options.include_text = true;
        const std::vector<std::string> lines =
            cli::RenderTurnView(*snapshot.view, cli::BuiltinTheme("dark"), options);
        std::lock_guard<std::mutex> stdout_lock(cli::StdoutWriteMutex());
        for (const std::string& line : lines) {
            cli::TermOut() << line << "\n";
        }
        registry.MarkMainPrinted(snapshot.revision);
    });
    CHECK(screen.VisibleText().find("offline final answer") != std::string::npos);
    CHECK(registry.MainLedgeSnapshot().live == false);
    registry.DetachMainTurn();
}

// ---------------------------------------------------------------------------
// P2(收拢写者,原子换页):调度器路的投影回归。
//   1) 提交不落笔:事件全走调度命令,Quiesce 后账画两讫;停表后 Emit
//      退化就地(与旧泵同款);
//   2) 快速往返:main 流式途中 A→B→A 抖 20 轮,每轮都有在飞 delta——
//      末帧只见当前页,旧 epoch 的 main 绘制不覆盖新帧;账一分不少;
//   3) 提交序:控制路(工具卡)不再由业务线程就地画,但正文先于工具卡
//      的次序在队列 FIFO 里保住。
// ---------------------------------------------------------------------------
TEST_CASE("P2 调度: 事件提交命令——账画两讫,停表后退化就地") {
    VirtualScreen screen;
    app::AgentViewRegistry registry;
    DispatchedHarness harness(&registry);
    registry.BeginMainTurn(harness.turn.collector.get(), &harness.turn.sink->CommitMutex());

    // 业务线程只提交(流内 delta 与控制路 usage 都进队列)。
    harness.turn.sink->Emit(MakeDelta("item-text", "dispatched paragraph.\n"));
    harness.turn.sink->Emit(MakeToolStart("item-tool", "toolu_1", "read_file"));
    harness.turn.sink->Emit(MakeToolDone("item-tool", "toolu_1", "read_file", "file body"));
    harness.turn.sink->Emit(MakeUsage());
    REQUIRE(WaitRevision(registry, 4));  // 收账在消费线程上走,修订号到位
    harness.dispatcher.Quiesce();        // 画面全落定

    const std::string visible = screen.VisibleText();
    CHECK(visible.find("dispatched paragraph") != std::string::npos);
    CHECK(visible.find("read_file") != std::string::npos);

    // 停表后迟到的 Emit:就地画(旧泵同款)——StopUiPump 返回即可见。
    harness.turn.sink->StopUiPump();
    harness.turn.sink->Emit(MakeDelta("item-text", "late inline tail.\n"));
    CHECK(screen.VisibleText().find("late inline tail") != std::string::npos);
    registry.DetachMainTurn();
}

TEST_CASE("P2 原子换页: 快速往返 20 轮——末帧只见当前页,旧 epoch 不覆盖新帧") {
    VirtualScreen screen;
    app::AgentViewRegistry registry;
    DispatchedHarness harness(&registry);
    registry.BeginMainTurn(harness.turn.collector.get(), &harness.turn.sink->CommitMutex());

    // 与生产同款的换页事务:统一提交锁内(调度 RunSync 的"排干+就地"在
    // 测试里由 WithMainRenderLock + 手工事务模拟)清可视区、切纪元、铺帧、
    // 钉水位。
    const auto switch_to_sub = [&](int id) {
        registry.WithMainRenderLock([&] {
            registry.SwitchViewed(id);
            std::lock_guard<std::mutex> stdout_lock(cli::StdoutWriteMutex());
            ClearViewportForViewSwitch();
            cli::TermOut() << "== sub agent #" << id << " view frame ==";
            cli::TermOut() << "\n";
            cli::TermOut().flush();
        });
    };
    const auto switch_to_main = [&] {
        registry.WithMainRenderLock([&] {
            registry.SwitchViewed(0);
            const app::AgentViewRegistry::MainTurnSnapshot snapshot = registry.TakeMainLedgeForRepaint();
            std::lock_guard<std::mutex> stdout_lock(cli::StdoutWriteMutex());
            ClearViewportForViewSwitch();
            PrintMainFrameLines(*snapshot.view);
            registry.MarkMainPrinted(snapshot.revision);
        });
    };

    // 20 轮往返:每轮先提交一批 main delta(在飞),随即换页——提交与
    // 换页的真实次序由调度线程竞着跑,任何交错下画面都不许串页。每枚
    // delta 带换行:一枚一行,不靠 80 列折行对齐(折行会把标记劈成两半,
    // find 落空——那是测试的账,不是产品的病)。
    for (int round = 0; round < 20; ++round) {
        harness.turn.sink->Emit(MakeDelta("item-text", "MAINWAVE" + std::to_string(round) + "\n"));
        if (round % 2 == 0) {
            switch_to_sub(7);
        } else {
            switch_to_main();
        }
    }
    // 收在 sub 页(偶数轮):末帧是 sub 的。Quiesce 之后所有提交都落定
    //(修订号多少取决于相邻同 item delta 在队尾并了几枚,不钉数值)。
    switch_to_sub(7);
    harness.dispatcher.Quiesce();
    REQUIRE(registry.MainRevision() >= 1);

    // 末帧只见当前页:视口内 sub 帧在、main 的字一个不见(旧帧已擦、
    // 在飞的旧 epoch 绘制被闸)。只断言视口行——清屏语义本就只清视口,
    // 滚出窗的内容留在滚屏历史是生产规矩,不算串页。视口行按当前视口顶
    // 换算:main 重铺随轮数长高,晚轮一笔未闸 delta 画过视口底时,帧账
    // "保锚可见"原语(EnsureViewportRowsForAnchorLocked)会平移视口跟住
    // 内容——长缓冲的生产语义。视口挪了,帧就落在 viewport_y 行而非
    // 0 行;钉死 0 行是把"视口恰好没挪过"当成了不变量(linux 腿首跑
    // 即红在这)。视口之上的旧行是滚屏历史,同样不算串页。
    const int viewport_top = screen.ViewportTop();
    CHECK(screen.RowText(viewport_top).has_value());
    CHECK(screen.RowText(viewport_top).value().find("sub agent #7 view frame") != std::string::npos);
    for (int row = viewport_top; row < viewport_top + VirtualScreen::kViewport; ++row) {
        const auto text = screen.RowText(row);
        if (!text.has_value()) {
            break;
        }
        for (int round = 0; round < 20; ++round) {
            CHECK(text.value().find("MAINWAVE" + std::to_string(round)) == std::string::npos);
        }
    }

    // 账一分不少:切回 main 重铺,20 轮 delta 全在终账里。
    switch_to_main();
    const std::string back = screen.VisibleText();
    for (int round = 0; round < 20; ++round) {
        CHECK(back.find("MAINWAVE" + std::to_string(round)) != std::string::npos);
    }
    harness.turn.sink->StopUiPump();
    registry.DetachMainTurn();
}

TEST_CASE("P2 提交序: 正文先于工具卡——FIFO 保住旧 DispatchInline 的次序钉") {
    VirtualScreen screen;
    app::AgentViewRegistry registry;
    DispatchedHarness harness(&registry);
    registry.BeginMainTurn(harness.turn.collector.get(), &harness.turn.sink->CommitMutex());

    // 先投正文(流内路),紧跟着控制路(工具起止):提交序即执行序,
    // 工具卡永远垫在已落笔的正文之后。
    harness.turn.sink->Emit(MakeDelta("item-text", "body before tool card.\n"));
    harness.turn.sink->Emit(MakeToolStart("item-tool", "toolu_1", "read_file"));
    harness.turn.sink->Emit(MakeToolDone("item-tool", "toolu_1", "read_file", "result"));
    REQUIRE(WaitRevision(registry, 3));
    harness.dispatcher.Quiesce();
    harness.turn.sink->StopUiPump();

    const std::string visible = screen.VisibleText();
    const std::size_t body_at = visible.find("body before tool card");
    const std::size_t tool_at = visible.find("read_file");
    CHECK(body_at != std::string::npos);
    CHECK(tool_at != std::string::npos);
    CHECK(body_at < tool_at);  // 正文先落笔
    registry.DetachMainTurn();
}

// ---------------------------------------------------------------------------
// P3 生命周期:会话换代(/clear、/resume)——旧世代的迟到帧令牌不写新会话,
// 审批悬账随换代按拒收口(future 不悬死工具线程);新世代从干净账开张。
// ---------------------------------------------------------------------------
TEST_CASE("P3 生命周期: 换代收口——旧令牌失配不写新会话,审批悬账随换代拒收") {
    VirtualScreen screen;
    app::AgentViewRegistry registry;
    {
        DispatchedHarness harness(&registry);
        registry.BeginMainTurn(harness.turn.collector.get(), &harness.turn.sink->CommitMutex());

        // 旧世代:main 页活画一笔。
        harness.turn.sink->Emit(MakeDelta("item-text", "old session sentence.\n"));
        REQUIRE(WaitRevision(registry, 1));
        CHECK(screen.VisibleText().find("old session sentence") != std::string::npos);

        // 旧世代挂一笔审批悬账(工具线程在 future 上等裁定),并取一枚
        // 换代前的帧令牌(锁外算好的旧帧)。
        cli::ApprovalChannel channel;
        channel.RegisterServer();
        auto stale_approval =
            channel.Submit(0, "write_file", [] { return true; }, registry.session_generation(), "t-old");
        REQUIRE(stale_approval.has_value());
        const cli::FrameToken old_token = registry.TokenFor(0);
        const std::uint64_t old_generation = registry.session_generation();

        // 换代(会话边界:BeginNewSession + 悬账按世代拒收,与会话侧接线同款)。
        registry.BeginNewSession();
        channel.DenyStaleGenerations(registry.session_generation());
        CHECK(registry.session_generation() != old_generation);
        CHECK(stale_approval->get() == false);  // 悬账按拒收口,不悬死
        CHECK(channel.PendingCount() == 0);

        // 旧帧令牌失配:锁外算好的旧帧写屏前核对不过——旧世代的迟到绘制
        // 不落新会话的屏。
        CHECK_FALSE(registry.TokenCurrent(old_token));
        CHECK(registry.TokenCurrent(registry.TokenFor(0)));  // 新世代新帧放行

        // 新世代开张:新回合账起头(修订号从零),新正文照常活画;旧句
        // 不重铺(世代册已作废)。
        registry.BeginMainTurn(harness.turn.collector.get(), &harness.turn.sink->CommitMutex());
        harness.turn.sink->Emit(MakeDelta("item-text", "new session sentence.\n"));
        REQUIRE(WaitRevision(registry, 1));
        CHECK(screen.VisibleText().find("new session sentence") != std::string::npos);
        const auto occurrences = [](const std::string& haystack, const std::string& needle) {
            std::size_t count = 0;
            for (std::size_t at = haystack.find(needle); at != std::string::npos;
                 at = haystack.find(needle, at + needle.size())) {
                ++count;
            }
            return count;
        };
        CHECK(occurrences(screen.VisibleText(), "old session sentence") == 1);  // 没被复活重铺

        registry.DetachMainTurn();
        channel.ClearServer();
    }
}
