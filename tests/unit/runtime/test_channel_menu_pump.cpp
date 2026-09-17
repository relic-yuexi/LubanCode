// QQ 接入单 Q7 总装册:假渠道 + 假模型 + 真渠道泵 —— 菜单/面板回调的
// 全链分派(todo §十三)。
//
// 验收对齐:
//   1) 菜单点击→路由→执行→回原会话:配对 sender 发"/查看任务"(控制命令)
//      → 零模型直答,任务清单(归属闸:只看自己的)投回原会话并锚定来信;
//   2) 预设 prompt:命中命令表 → 模型恰一次且请求正文 = 预设输入(不是
//      用户点菜单的原文);require_tools 过五层闸;
//   3) 权限外菜单项:require_tools 名单外 → 稳定拒绝正文,零模型;
//   4) 未配对点击:dm_policy=pairing 下未知 sender 点全局菜单 → 配对提示
//      (带一次性码),零模型;
//   5) 手敲同名命令走同一分派(带空白也命中)。
// 真平台菜单触达/回调形状归 Q3 真机未验——本地 mock 只钉宿主分派链。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "channel/manager.hpp"
#include "channel/types.hpp"
#include "fake_channel_sidecar.hpp"
#include "gateway/automation_store.hpp"
#include "gateway/profile.hpp"
#include "gateway/reply_outbox.hpp"
#include "runtime/automation_pump.hpp"
#include "runtime/channel_automation.hpp"
#include "runtime/channel_work_pump.hpp"
#include "runtime/headless_executor.hpp"
#include "runtime/channel_file_delivery.hpp"
#include "tools/path_utils.hpp"
#include "tools/tool.hpp"
#include "workspace/identity.hpp"

using namespace lubancode;
using lubancode::test_support::FakeChannelSidecar;

namespace {

struct EnvGuard {
    explicit EnvGuard(const char* name, const char* value) : name_(name) {
#ifdef _WIN32
        _putenv((std::string(name_) + "=" + value).c_str());
#else
        setenv(name_, value, 1);
#endif
    }
    ~EnvGuard() {
#ifdef _WIN32
        _putenv((std::string(name_) + "=").c_str());
#else
        unsetenv(name_);
#endif
    }
    const char* name_;
};

void CountCall(const std::filesystem::path& counter, const std::string& who) {
    std::error_code ec;
    std::filesystem::create_directories(counter.parent_path(), ec);
    std::ofstream stream(counter, std::ios::binary | std::ios::app);
    stream << who << "\n";
}

std::size_t CountOf(const std::filesystem::path& counter, const std::string& who) {
    std::ifstream stream(counter, std::ios::binary);
    std::string line;
    std::size_t count = 0;
    while (std::getline(stream, line)) {
        if (line == who) ++count;
    }
    return count;
}

// 逐请求抓正文:预设 prompt 的断言看真请求不靠猜。
class CaptureBackend : public api::Backend {
public:
    CaptureBackend(std::filesystem::path counter, std::vector<std::vector<api::StreamEvent>> scripts)
        : counter_(std::move(counter)), scripts_(std::move(scripts)) {}

    std::expected<void, api::Error> send_stream(
        const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        if (cancel != nullptr && cancel->load()) {
            return std::unexpected(api::Error{api::ErrorKind::Cancelled, "cancelled"});
        }
        CountCall(counter_, "model");
        std::size_t users = 0, images = 0;
        for (const auto& message : request.messages) {
            if (message.role == api::Role::User) ++users;
            for (const auto& block : message.content)
                if (std::holds_alternative<api::ImageBlock>(block)) ++images;
            for (const auto& block : message.content) {
                if (const auto* text = std::get_if<api::TextBlock>(&block))
                    saw_host_clock = saw_host_clock || text->text.find("宿主时钟") != std::string::npos;
            }
        }
        user_counts.push_back(users);
        image_counts.push_back(images);
        systems.push_back(request.system);
        // 抓最后一条 user 消息的文本块。
        for (auto it = request.messages.rbegin(); it != request.messages.rend(); ++it) {
            if (it->role != api::Role::User) {
                continue;
            }
            for (const auto& block : it->content) {
                if (const auto* text = std::get_if<api::TextBlock>(&block)) {
                    last_user_texts.push_back(text->text);
                    break;
                }
            }
            break;
        }
        if (calls_ >= scripts_.size()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "script exhausted"});
        }
        for (const auto& event : scripts_[calls_]) {
            on_event(event);
        }
        ++calls_;
        return {};
    }

    std::vector<std::string> last_user_texts;
    std::vector<std::size_t> user_counts, image_counts;
    std::vector<std::string> systems;
    bool saw_host_clock = false;

private:
    std::filesystem::path counter_;
    std::vector<std::vector<api::StreamEvent>> scripts_;
    std::size_t calls_ = 0;
};

std::vector<api::StreamEvent> TextScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "test-model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

std::vector<api::StreamEvent> ToolUseScript(const std::string& tool_id, const std::string& name,
                                            const std::string& input_json) {
    return {
        api::MessageStart{"msg", "test-model"},
        api::ToolUseStart{0, tool_id, name},
        api::ToolUseInputDelta{0, input_json},
        api::ContentBlockDone{0},
        api::MessageDone{"tool_use", api::Usage{}},
    };
}

class FakeTransport final : public channel::ChannelBridgeTransport {
public:
    explicit FakeTransport(FakeChannelSidecar& sidecar) : sidecar_(sidecar) {}
    void WriteToSidecar(const std::byte* data, std::size_t size) override {
        sidecar_.FeedFromHost(data, size);
    }
    std::vector<std::byte> DrainFromSidecar() override { return sidecar_.DrainToHost(); }

private:
    FakeChannelSidecar& sidecar_;
};

channel::ChannelInboundEvent MakeDmAt(const std::string& delivery_id,
                                      const std::string& provider_event_id,
                                      const std::string& conversation, const std::string& text,
                                      const std::string& message_id, std::int64_t received_at_ms) {
    channel::ChannelInboundEvent event;
    event.delivery_id = delivery_id;
    event.provider_event_id = provider_event_id;
    event.channel_id = "qqbot";
    event.account_id = "main";
    event.received_at_ms = received_at_ms;
    event.provider_at_ms = received_at_ms;
    event.conversation.kind = channel::ConversationKind::Direct;
    event.conversation.id = conversation;
    event.sender.id = "sender-" + conversation;
    event.sender.display_name = "用户";
    event.message_id = message_id;
    channel::ChannelPart part;
    part.type = channel::ChannelPartType::Text;
    part.text = text;
    event.parts.push_back(part);
    return event;
}

// Q7 命令表:控制命令三路 + 预设输入两路(一路在权限内,一路在权限外)。
std::vector<channel::ChannelCommandBindingUserConfig> MenuCommands() {
    using channel::ChannelCommandBindingUserConfig;
    std::vector<ChannelCommandBindingUserConfig> commands;
    ChannelCommandBindingUserConfig help;
    help.match = "/帮助";
    help.action = "help";
    commands.push_back(help);
    ChannelCommandBindingUserConfig file_help;
    file_help.match = "/文件说明";
    file_help.action = "file_help";
    commands.push_back(file_help);
    ChannelCommandBindingUserConfig list;
    list.match = "/查看任务";
    list.action = "list_reminders";
    commands.push_back(list);
    ChannelCommandBindingUserConfig preset;
    preset.match = "/整理";
    preset.action = "prompt";
    preset.prompt = "请把我的待办整理成清单,并给出今天的三件要事。";
    preset.require_tools = {"create_reminder", "list_reminders"};
    commands.push_back(preset);
    ChannelCommandBindingUserConfig denied;
    denied.match = "/受限";
    denied.action = "prompt";
    denied.prompt = "受限动作的预设输入";
    denied.require_tools = {"run_command"};
    commands.push_back(denied);
    return commands;
}

// 纵向装配(与 Q5 册同骨架):manager(真 Bridge 帧)+ automation 泵
// (outbox/store 单写者)+ 渠道泵 + 任务桥。
struct MenuPumpFixture {
    std::filesystem::path root;
    gateway::GatewayProfilePaths paths;
    std::filesystem::path workspaces_root;
    std::filesystem::path channels_root;
    std::filesystem::path ws_root;
    std::filesystem::path counter_file;
    std::int64_t now = 1724700000000;
    std::vector<std::vector<api::StreamEvent>> scripts;

    FakeChannelSidecar sidecar;
    FakeTransport transport{sidecar};
    std::unique_ptr<CaptureBackend> backend;
    std::unique_ptr<channel::ChannelManager> manager;
    std::optional<runtime::GatewayAutomationPump> automation;
    std::shared_ptr<runtime::ChannelAutomationBridge> bridge;
    std::optional<runtime::ChannelWorkPump> pump;
    workspace::WorkspaceIdentity identity;

    explicit MenuPumpFixture(const char* tag, channel::DmPolicy dm_policy)
        : root(std::filesystem::temp_directory_path() /
               ("lubancode-q7-menu-" + std::string(tag))) {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root, ec);
        paths = gateway::ResolveGatewayProfilePaths(root / "gateway", "default");
        workspaces_root = root / "workspaces";
        channels_root = root / "channels";
        ws_root = root / "ws";
        counter_file = root / "calls.log";
        std::filesystem::create_directories(ws_root, ec);

        channel::ChannelManagerOptions manager_options;
        manager_options.state_root = channels_root;
        manager_options.now_ms = [this] { return now; };
        manager_options.alive_checker = [](unsigned long) { return true; };
        manager = std::make_unique<channel::ChannelManager>(std::move(manager_options));

        channel::ChannelAccountUserConfig config;
        config.enabled = true;
        config.transport = "websocket";
        config.secret_env = "QQBOT_SECRET";
        config.dm_policy = dm_policy;
        config.tools.allow = {"create_reminder", "list_reminders", "cancel_reminder", "send_file"};
        config.commands = MenuCommands();
        REQUIRE(manager->AddAccount("qqbot", "main", config, &transport).status ==
                channel::ChannelManager::AddAccountResult::Status::Ok);
        REQUIRE_FALSE(manager->StartAccount("qqbot", "main").has_value());
        manager->Pump("qqbot", "main");
        manager->Pump("qqbot", "main");
        REQUIRE(manager->Snapshot("qqbot", "main")->state ==
                channel::ChannelAccountState::Running);
        identity = workspace::MakeFallbackIdentity(ws_root);
    }

    bool OpenPumps(tools::ToolRegistry& registry) {
        backend = std::make_unique<CaptureBackend>(counter_file, scripts);
        runtime::GatewayAutomationPump::Options pump_options;
        pump_options.paths = paths;
        pump_options.workspaces_root = workspaces_root;
        pump_options.workspace_identity = identity;
        pump_options.cwd_utf8 = tools::PathToUtf8(root);
        pump_options.lubancode_version = "0.26.267-test";
        pump_options.wire_name = "test-wire";
        pump_options.model = "test-model";
        pump_options.max_steps_per_turn = 8;
        automation.emplace();
        const auto open = runtime::GatewayAutomationPump::Open(&*automation, *backend, registry,
                                                               std::move(pump_options));
        REQUIRE_MESSAGE(open.ok, open.error);
        if (!open.ok) {
            return false;
        }
        automation->set_owner_epoch("q7-test-epoch");
        bridge = std::make_shared<runtime::ChannelAutomationBridge>(automation->store(),
                                                                    [this] { return now; });
        runtime::RegisterChannelAutomationTools(registry, bridge);
        runtime::RegisterChannelFileTool(registry);
        runtime::ChannelWorkPump::Options work_options;
        work_options.manager = manager.get();
        work_options.outbox = automation->outbox();
        work_options.channels_state_root = channels_root;
        work_options.workspaces_root = workspaces_root;
        work_options.workspace_identity = identity;
        work_options.cwd_utf8 = tools::PathToUtf8(root);
        work_options.lubancode_version = "0.26.267-test";
        work_options.wire_name = "test-wire";
        work_options.model = "test-model";
        work_options.tools.allow = {"create_reminder", "list_reminders", "cancel_reminder", "send_file"};
        work_options.max_steps_per_turn = 8;
        work_options.skills_prompt = "Available skill: demo-skill";
        work_options.media_download = [](const std::string&) -> std::expected<runtime::ChannelMediaBytes, std::string> {
            return runtime::ChannelMediaBytes{std::string("GIF89a\x01\x00\x01\x00\x00\x00\x00", 13)};
        };
        work_options.automation_store = automation->store();
        work_options.automation_bridge = bridge;
        pump.emplace();
        const auto work_open =
            runtime::ChannelWorkPump::Open(&*pump, *backend, registry, std::move(work_options));
        REQUIRE_MESSAGE(work_open.ok, work_open.error);
        if (!work_open.ok) {
            return false;
        }
        pump->set_owner_epoch("q7-test-epoch");
        return true;
    }

    bool Tick(std::int64_t step_ms = 100) {
        const bool automation_ok = automation->TickOnce(now);
        const bool channel_ok = pump->TickOnce(now);
        now += step_ms;
        return automation_ok && channel_ok;
    }

    void EmitAndIngest(const channel::ChannelInboundEvent& event) {
        sidecar.EmitInboundEvent(event);
        manager->Pump("qqbot", "main");
    }

    void TickUntilQuiet(int max_ticks = 40) {
        for (int i = 0; i < max_ticks; ++i) {
            Tick();
        }
    }

    std::string SentTextAt(std::size_t index) {
        REQUIRE(fixture_index_ok(index));
        return sidecar.sent_messages()[index].params["parts"][0]["text"].get<std::string>();
    }

    bool fixture_index_ok(std::size_t index) const {
        return index < sidecar.sent_messages().size();
    }

    gateway::AutomationStore* store() { return automation->store(); }
};

const char* kCreateCronInput =
    "{\"description\":\"每天九点提醒我喝水\",\"cron\":\"0 9 * * *\","
    "\"timezone\":\"Asia/Shanghai\"}";

}  // namespace

// ---------------------------------------------------------------------------
// 验收 1+2+3+5:配对 sender 的菜单回调全链
// ---------------------------------------------------------------------------

TEST_CASE("菜单点击全链:控制命令零模型直答;预设 prompt 进模型;权限外拒") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    MenuPumpFixture fixture("full-chain", channel::DmPolicy::Open);
    fixture.scripts = {
        ToolUseScript("tu-1", "create_reminder", kCreateCronInput),
        TextScript("已设置:每天早上九点提醒你喝水。"),
        TextScript("整理完成:今天的三件要事如下。"),
    };
    tools::ToolRegistry registry;
    REQUIRE(fixture.OpenPumps(registry));

    // 0) 先用普通消息建一笔任务(归属 sender-dm-a)。
    fixture.EmitAndIngest(
        MakeDmAt("in-1", "pe-1", "dm-a", "每天九点提醒我喝水", "m-1", fixture.now));
    REQUIRE(fixture.Tick());
    fixture.TickUntilQuiet();
    REQUIRE(CountOf(fixture.counter_file, "model") == 2);
    REQUIRE(fixture.store()->ListJobs().size() == 1);

    // 1) 菜单点击(填入+发送)"/查看任务" → 控制命令零模型直答,清单只含
    //    自己的任务,投回原会话并锚定来信。
    fixture.EmitAndIngest(MakeDmAt("in-2", "pe-2", "dm-a", "/查看任务", "m-2", fixture.now));
    REQUIRE(fixture.Tick());
    fixture.TickUntilQuiet();
    REQUIRE(CountOf(fixture.counter_file, "model") == 2);  // 零模型
    REQUIRE(fixture.sidecar.sent_messages().size() == 2);
    CHECK(fixture.sidecar.sent_messages()[1].params["reply_to_message_id"] == "m-2");
    const std::string list_reply = fixture.SentTextAt(1);
    CHECK(list_reply.find("job-") != std::string::npos);
    CHECK(list_reply.find("喝水") != std::string::npos);
    CHECK(list_reply.find("进行中") != std::string::npos);

    // 2) 他人(dm-b)点同一菜单:归属闸——看不到 dm-a 的任务;手敲同名命令
    //    (带首尾空白)走同一分派。
    fixture.EmitAndIngest(
        MakeDmAt("in-3", "pe-3", "dm-b", "  /查看任务 \r\n", "m-3", fixture.now));
    REQUIRE(fixture.Tick());
    fixture.TickUntilQuiet();
    REQUIRE(CountOf(fixture.counter_file, "model") == 2);
    REQUIRE(fixture.sidecar.sent_messages().size() == 3);
    const std::string other_reply = fixture.SentTextAt(2);
    CHECK(other_reply.find("还没有") != std::string::npos);

    // 3) 预设 prompt("/整理"):权限内 → 模型恰一次,请求正文 = 预设输入
    //    (不是用户点的原文)。
    fixture.EmitAndIngest(MakeDmAt("in-4", "pe-4", "dm-a", "/整理", "m-4", fixture.now));
    REQUIRE(fixture.Tick());
    fixture.TickUntilQuiet();
    REQUIRE(CountOf(fixture.counter_file, "model") == 3);
    REQUIRE(fixture.sidecar.sent_messages().size() == 4);
    REQUIRE(fixture.backend->last_user_texts.size() >= 1);
    const std::string& model_input = fixture.backend->last_user_texts.back();
    CHECK(model_input.find("请把我的待办整理成清单") != std::string::npos);
    CHECK(model_input.find("/整理") == std::string::npos);  // 原文被预设替换

    // 4) 权限外菜单项("/受限" 绑 run_command,不在五层交集名单)→ 稳定
    //    拒绝正文,零模型。
    fixture.EmitAndIngest(MakeDmAt("in-5", "pe-5", "dm-a", "/受限", "m-5", fixture.now));
    REQUIRE(fixture.Tick());
    fixture.TickUntilQuiet();
    REQUIRE(CountOf(fixture.counter_file, "model") == 3);
    REQUIRE(fixture.sidecar.sent_messages().size() == 5);
    const std::string denied_reply = fixture.SentTextAt(4);
    CHECK(denied_reply.find("授权范围") != std::string::npos);
    CHECK(denied_reply.find("run_command") != std::string::npos);

    // 5) "/帮助" → 宿主生成的帮助正文(命令表自举,零模型)。
    fixture.EmitAndIngest(MakeDmAt("in-6", "pe-6", "dm-a", "/帮助", "m-6", fixture.now));
    REQUIRE(fixture.Tick());
    fixture.TickUntilQuiet();
    REQUIRE(CountOf(fixture.counter_file, "model") == 3);
    REQUIRE(fixture.sidecar.sent_messages().size() == 6);
    const std::string help_reply = fixture.SentTextAt(5);
    CHECK(help_reply.find("/查看任务") != std::string::npos);
    CHECK(help_reply.find("/整理") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 验收 4:未配对用户点全局菜单 → 配对提示,零模型
// ---------------------------------------------------------------------------

TEST_CASE("未配对点击:全局菜单对所有人可见,但宿主分派要配对 sender") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    MenuPumpFixture fixture("unpaired", channel::DmPolicy::Pairing);
    fixture.scripts = {};  // 零模型:任何模型调用都会耗尽脚本而失败
    tools::ToolRegistry registry;
    REQUIRE(fixture.OpenPumps(registry));

    fixture.EmitAndIngest(MakeDmAt("in-1", "pe-1", "dm-x", "/帮助", "m-1", fixture.now));
    REQUIRE(fixture.Tick());
    fixture.TickUntilQuiet();
    REQUIRE(CountOf(fixture.counter_file, "model") == 0);
    // 收到的是配对提示(一次性码 + approve 指引),不是帮助正文。
    REQUIRE(fixture.sidecar.sent_messages().size() == 1);
    const std::string notice = fixture.SentTextAt(0);
    CHECK(notice.find("配对") != std::string::npos);
    CHECK(notice.find("我能做这些") == std::string::npos);
    // ingress 落 rejected(pairing_pending),不回捞不执行。
    const auto records = fixture.manager->IngressRecords("qqbot", "main");
    REQUIRE(records.size() == 1);
    CHECK(channel::IngressEventStateName(records[0].state) == "rejected");
}

TEST_CASE("QQ builtins manage isolated contexts without sending slash commands to model") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    MenuPumpFixture fixture("builtin-sessions", channel::DmPolicy::Open);
    fixture.scripts = {TextScript("one"), TextScript("two"), TextScript("three")};
    tools::ToolRegistry registry;
    REQUIRE(fixture.OpenPumps(registry));
    int id = 0;
    const auto send = [&](const std::string& text) {
        const auto key = "builtin-" + std::to_string(++id);
        fixture.EmitAndIngest(MakeDmAt(key, key, "dm-a", text, key, fixture.now));
        REQUIRE(fixture.Tick());
        fixture.TickUntilQuiet();
    };
    send("remember alpha");
    const auto first = fixture.pump->session_id_for("qqbot", "main", "dm-a");
    REQUIRE_FALSE(first.empty());
    send("/help");
    send("/new");
    send("fresh question");
    CHECK(fixture.backend->user_counts == std::vector<std::size_t>{1, 1});
    send("/session switch default");
    send("remember previous");
    CHECK(fixture.backend->user_counts == std::vector<std::size_t>{1, 1, 2});
    send("/session switch foreign-id");
    send("/not-implemented");
    send("/status");
    CHECK(CountOf(fixture.counter_file, "model") == 3);
    CHECK(fixture.SentTextAt(6).find("找不到") != std::string::npos);
    CHECK(fixture.SentTextAt(7).find("暂不支持") != std::string::npos);
    CHECK(fixture.SentTextAt(8).find("未装配") != std::string::npos);
    CHECK_FALSE(fixture.backend->saw_host_clock);
    CHECK(fixture.backend->systems[0].find("get_current_time") != std::string::npos);
}

TEST_CASE("QQ accepted pictures reach provider input and generated files enter outbox") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    MenuPumpFixture fixture("vision-file", channel::DmPolicy::Open);
    fixture.scripts = {TextScript("picture received"),
        ToolUseScript("send-1", "send_file", R"({"path":"report.txt"})"),
        TextScript("文件已暂存，随回复投递。")};
    tools::ToolRegistry registry;
    REQUIRE(fixture.OpenPumps(registry));
    std::ofstream(fixture.root / "report.txt") << "report content";
    auto event = MakeDmAt("image-1", "image-1", "dm-a", "read image", "image-1", fixture.now);
    channel::ChannelPart part;
    part.type = channel::ChannelPartType::Image;
    part.remote_ref = "https://example.qq.com/image";
    part.mime_type = "image/gif";
    part.file_name = "test.gif";
    event.parts.push_back(part);
    fixture.EmitAndIngest(event);
    REQUIRE(fixture.Tick());
    fixture.TickUntilQuiet();
    REQUIRE(fixture.backend->image_counts.size() == 1);
    CHECK(fixture.backend->image_counts[0] == 1);
    CHECK(fixture.backend->systems[0].find("demo-skill") != std::string::npos);
    fixture.EmitAndIngest(MakeDmAt("file-2", "file-2", "dm-a", "send report", "file-2", fixture.now));
    REQUIRE(fixture.Tick());
    fixture.TickUntilQuiet();
    bool found = false;
    for (const auto& item : fixture.automation->outbox()->ListItems()) {
        if (item.attachment_file_name == "report.txt") {
            found = true;
            CHECK_FALSE(item.attachment_local_path.empty());
        }
    }
    CHECK(found);
    CHECK(CountOf(fixture.counter_file, "model") == 3);
}
