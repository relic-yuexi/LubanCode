// 前缀缓存守恒单(§五 B):宿主目录通知的 SessionV3 落账。
//
// 断点背景:worktree enter/exit 改 cwd 后重拼 system,目录一换 system 指纹
// 变,前缀缓存报废(实测 86%→30%)。修法把目录更新改为"宿主追加的持久化
// 目录通知":role=user、purpose=conversation、origin=session_runtime,经
// TrajectorySessionLedger::RecordHostDirectoryNotice 落账。本册钉:
//   1. 主路全流:通知消息进账(origin=session_runtime 不冒充人类输入)→
//      context.input.applied 接纳进链 → 真实回合的 prepared.
//      inputMessageRefs 沿链带上(下一请求必见新目录)。验卷收尾。
//   2. 回合署名:主回合开着挂当前 turnId;空闲切换(slash 路)走 writer
//      自家号池(schema:user 消息 turnId 必填)。
//   3. 两次切换两条通知,链序如实;"一次成功切换只追加一次"由调用侧
//      保证——本册钉"再调再落"的账面行为(不吞不并)。
//   4. resume 口径:链投影(EffectiveConversationFromV3)把通知折成 user
//      消息——恢复重放不丢通知、不重复投递(链身份即去重)。
//   5. 正文纯函数:落账与内存注入共用同一只 FormatHostDirectoryNoticeText,
//      旧目录/新目录/原因/来源标识四要素齐。
#include <doctest/doctest.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/loop.hpp"  // RequestPreparedContext(prepared 前缀账的载荷)
#include "api/types.hpp"
#include "runtime/trajectory_session.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/replay.hpp"                // ReplayMessage
#include "trajectory/session_manager.hpp"  // EffectiveConversationFromV3
#include "trajectory/v3/envelope.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/session_switch.hpp"  // FindV3SessionStream
#include "trajectory/v3/writer.hpp"          // VerifyV3File
#include "workspace/identity.hpp"

using namespace lubancode;
using lubancode::runtime::TrajectorySessionLedger;
using lubancode::runtime::TrajectoryTurnBridge;

namespace {

namespace fs = std::filesystem;

// ctest 钉 LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS=0;v3 册显式开回 1(同款见
// test_v3_memory_recall_bridge.cpp)。
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

fs::path FreshRoot(const char* tag) {
    const auto dir = fs::temp_directory_path() / ("lubancode-v3-host-cwd-notice-" + std::string(tag));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

TrajectorySessionLedger::Options LedgerOptions(const fs::path& root) {
    TrajectorySessionLedger::Options options;
    options.workspaces_root = root / "workspaces";
    options.workspace_root = root / "repo";
    options.workspace_identity = lubancode::workspace::MakeFallbackIdentity(root / "repo");
    options.launch_cwd = "D:/tmp/repo";
    options.lubancode_version = "0.26.278-test";
    options.v3_system_content = "你是 LubanCode,读写跑都走工具。";
    return options;
}

std::vector<nlohmann::json> StreamLines(const fs::path& stream) {
    std::vector<nlohmann::json> lines;
    const auto raw = trajectory::ReadJournalLines(stream);
    REQUIRE(raw.has_value());
    for (const std::string& line : *raw) {
        lines.push_back(nlohmann::json::parse(line, nullptr, false));
        REQUIRE_FALSE(lines.back().is_discarded());
    }
    return lines;
}

// 宿主目录通知的正式消息行(origin=session_runtime,区别于人类输入与
// 工具输出;工具正文/仓库文件伪造不了这只 origin——写侧只有宿主口)。
const nlohmann::json* FindNoticeMessage(const std::vector<nlohmann::json>& lines) {
    for (const auto& line : lines) {
        if (line.value("type", std::string()) != "message") continue;
        if (line.value("origin", std::string()) != "session_runtime") continue;
        if (line.value("purpose", std::string()) != "conversation") continue;
        return &line;
    }
    return nullptr;
}

api::Message UserMessage(const std::string& text) {
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(api::TextBlock{text});
    return message;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. 主路全流:通知进账 → 进链 → 下一请求的输入引用带上
// ---------------------------------------------------------------------------
TEST_CASE("v3 主路: 宿主目录通知进账进链,prepared 引用沿链带上") {
    EnvGuard v3gate("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("main");

    auto opened = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(opened.has_value());
    TrajectorySessionLedger& ledger = *opened;
    REQUIRE(ledger.v3_main_writer() != nullptr);

    // 空闲切换(slash 路):主回合没开,通知自起新回合号署名。
    const std::string error = ledger.RecordHostDirectoryNotice(
        "D:/tmp/repo", "D:/tmp/repo/.lubancode/worktrees/kanban", "user /worktree new");
    CHECK(error.empty());

    // 真实主回合:下一请求必见通知(inputMessageRefs 沿链带上)。
    auto turn = ledger.NewTurnBridge(TrajectoryTurnBridge::Identity{"kimi", "responses", "terminal"});
    REQUIRE(turn != nullptr);
    turn->BeginTurn("turn-1", "external_user");
    const api::Message user = UserMessage("继续干活");
    turn->RecordInput(user);
    api::Request request;
    request.model = "kimi-k2.6";
    request.system = LedgerOptions(root).v3_system_content;
    request.messages.push_back(user);
    const std::string request_id = turn->OnRequestPrepared(request, agent::RequestPreparedContext{});
    REQUIRE_FALSE(request_id.empty());

    const auto stream = trajectory::v3::FindV3SessionStream(ledger.session_dir());
    REQUIRE(stream.has_value());
    const auto lines = StreamLines(*stream);

    // (a) 通知消息:origin=session_runtime、display=visible、正文四要素。
    const nlohmann::json* notice = FindNoticeMessage(lines);
    REQUIRE(notice != nullptr);
    CHECK(notice->at("message").value("role", std::string()) == "user");
    CHECK_FALSE(notice->value("turnId", std::string()).empty());  // schema:user 必填
    REQUIRE(notice->contains("display"));
    CHECK(notice->at("display").at("mode").get<std::string>() == "visible");
    const std::string text = notice->at("message").at("content").get<std::string>();
    CHECK(text == lubancode::runtime::FormatHostDirectoryNoticeText(
                     "D:/tmp/repo", "D:/tmp/repo/.lubancode/worktrees/kanban", "user /worktree new"));
    CHECK(text.find("D:/tmp/repo/.lubancode/worktrees/kanban") != std::string::npos);
    CHECK(text.find("[宿主通知]") != std::string());
    const std::string notice_id = notice->value("messageId", std::string());

    // (b) 链接纳:context.input.applied 点名通知消息。
    bool admitted = false;
    for (const auto& line : lines) {
        if (line.value("type", std::string()) != "event") continue;
        if (line.value("kind", std::string()) != "context.input.applied") continue;
        for (const auto& ref : line.at("payload").at("addedMessageRefs")) {
            if (ref.get<std::string>() == notice_id) admitted = true;
        }
    }
    CHECK(admitted);

    // (c) 下一请求的输入引用:通知在链上,prepared 带上(排在 user 之前,
    //     链序如实——通知先落账)。
    const nlohmann::json* prepared = nullptr;
    for (const auto& line : lines) {
        if (line.value("type", std::string()) != "event") continue;
        if (line.value("kind", std::string()) != "model.request.prepared") continue;
        if (line.value("requestId", std::string()) != request_id) continue;
        prepared = &line;
    }
    REQUIRE(prepared != nullptr);
    const auto& input_refs = prepared->at("payload").at("inputMessageRefs");
    REQUIRE(input_refs.size() == 2);
    CHECK(input_refs[0].get<std::string>() == notice_id);

    // (d) prepared 的持久化请求账(§五 D):没接前缀账的调用不落块;接了
    //     的(epoch/追加律/断因)完整落——不再只留内存统计。仍在同一回合
    //     内发第二枚请求(收口前),账面同链。
    REQUIRE(prepared->at("payload").contains("prefixAccount") == false);
    {
        agent::RequestPreparedContext prefix_ctx;
        prefix_ctx.has_prefix_account = true;
        prefix_ctx.cache_epoch = 3;
        prefix_ctx.prefix_append_only = false;
        prefix_ctx.epoch_break_reason = "system_changed";
        prefix_ctx.system_hash = "deadbeef";
        prefix_ctx.tools_hash = "cafebabe";
        api::Request second;
        second.model = "kimi-k2.6";
        second.system = LedgerOptions(root).v3_system_content;
        second.messages.push_back(user);
        const std::string second_id = turn->OnRequestPrepared(second, prefix_ctx);
        REQUIRE_FALSE(second_id.empty());
        const nlohmann::json* second_prepared = nullptr;
        for (const auto& line : StreamLines(*stream)) {
            if (line.value("type", std::string()) != "event") continue;
            if (line.value("kind", std::string()) != "model.request.prepared") continue;
            if (line.value("requestId", std::string()) != second_id) continue;
            second_prepared = &line;
        }
        REQUIRE(second_prepared != nullptr);
        const auto& account = second_prepared->at("payload").at("prefixAccount");
        CHECK(account.value("cacheEpoch", std::uint64_t{0}) == 3);
        CHECK(account.value("appendOnly", true) == false);
        CHECK(account.value("epochBreakReason", std::string()) == "system_changed");
        CHECK(account.value("systemHash", std::string()) == "deadbeef");
        CHECK(account.value("toolsHash", std::string()) == "cafebabe");
    }
    turn->EndTurn(/*ok=*/true, /*cancelled=*/false, "");

    // (e) 验卷:整卷哈希链不破。
    const auto report = trajectory::v3::VerifyV3File(*stream);
    CHECK(report.ok);
    CHECK(report.context.chain.size() == 3);  // system 根 + 通知 + user
}

// ---------------------------------------------------------------------------
// 2. 回合署名:turn 开着挂当前回合;空闲切换走 writer 号池
// ---------------------------------------------------------------------------
TEST_CASE("v3 回合署名: turn 内切换挂当前 turnId,空闲切换走 writer 号池") {
    EnvGuard v3gate("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("turnid");

    auto opened = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(opened.has_value());
    TrajectorySessionLedger& ledger = *opened;

    // 工具触发的 enter/exit:主回合开着,通知署当前回合号。
    auto turn = ledger.NewTurnBridge(TrajectoryTurnBridge::Identity{"kimi", "responses", "terminal"});
    REQUIRE(turn != nullptr);
    turn->BeginTurn("turn-7", "external_user");
    CHECK(ledger.RecordHostDirectoryNotice("D:/tmp/repo", "D:/tmp/repo/.lubancode/worktrees/w1",
                                           "model worktree enter")
              .empty());
    turn->EndTurn(/*ok=*/true, /*cancelled=*/false, "");

    // 空闲 slash 路:writer 自家号池兜底(zero-pad,与宿主 turn-<n> 不撞名)。
    CHECK(ledger.RecordHostDirectoryNotice("D:/tmp/repo/.lubancode/worktrees/w1", "D:/tmp/repo",
                                           "user /worktree exit")
              .empty());

    const auto stream = trajectory::v3::FindV3SessionStream(ledger.session_dir());
    REQUIRE(stream.has_value());
    const auto lines = StreamLines(*stream);
    std::vector<const nlohmann::json*> notices;
    for (const auto& line : lines) {
        if (line.value("type", std::string()) != "message") continue;
        if (line.value("origin", std::string()) != "session_runtime") continue;
        notices.push_back(&line);
    }
    REQUIRE(notices.size() == 2);
    CHECK(notices[0]->value("turnId", std::string()) == "turn-7");
    CHECK(notices[1]->value("turnId", std::string()).rfind("turn-00000", 0) == 0);
    // 两次切换两条通知,链序如实(切出再切回也不吞不并)。
    CHECK(notices[0]->at("message").at("content").get<std::string>().find("w1") != std::string::npos);
    CHECK(notices[1]->at("message").at("content").get<std::string>().find("D:/tmp/repo") !=
          std::string::npos);
    const auto report = trajectory::v3::VerifyV3File(*stream);
    CHECK(report.ok);
}

// ---------------------------------------------------------------------------
// 3. resume 口径:链投影把通知折成 user 消息,恢复不丢不重
// ---------------------------------------------------------------------------
TEST_CASE("v3 resume 投影: 通知折进有效对话,恢复重放不丢不重") {
    EnvGuard v3gate("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("resume");

    auto opened = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(opened.has_value());
    TrajectorySessionLedger& ledger = *opened;

    CHECK(ledger
              .RecordHostDirectoryNotice("D:/tmp/repo", "D:/tmp/repo/.lubancode/worktrees/board",
                                         "model worktree enter")
              .empty());
    auto turn = ledger.NewTurnBridge(TrajectoryTurnBridge::Identity{"kimi", "responses", "terminal"});
    REQUIRE(turn != nullptr);
    turn->BeginTurn("turn-1", "external_user");
    turn->RecordInput(UserMessage("在房里跑测试"));
    turn->EndTurn(/*ok=*/true, /*cancelled=*/false, "");

    // resume 读侧:链投影含通知文本,恰好一条(链身份即去重)。
    const auto stream = trajectory::v3::FindV3SessionStream(ledger.session_dir());
    REQUIRE(stream.has_value());
    const auto ledger_read = trajectory::v3::ReadV3Ledger(*stream);
    REQUIRE(ledger_read.has_value());
    const auto conversation = trajectory::EffectiveConversationFromV3(
        *ledger_read, trajectory::v3::ProjectModelContext(*ledger_read));
    std::size_t notice_count = 0;
    bool notice_text_ok = false;
    for (const auto& message : conversation) {
        if (message.role != trajectory::ReplayMessage::Role::User) continue;
        for (const auto& block : message.blocks) {
            if (!block.is_object() || block.value("type", std::string()) != "text") continue;
            const std::string& text = block.at("text").get<std::string>();
            if (text.find("[宿主通知]") != std::string::npos) {
                ++notice_count;
                notice_text_ok = text.find("worktrees/board") != std::string::npos &&
                                 text.find("后续相对路径与命令默认在当前目录执行") != std::string::npos;
            }
        }
    }
    CHECK(notice_count == 1);
    CHECK(notice_text_ok);
}

// ---------------------------------------------------------------------------
// 4. 正文纯函数:落账与内存注入共用,四要素齐
// ---------------------------------------------------------------------------
TEST_CASE("正文纯函数: 旧目录/新目录/原因/来源标识四要素") {
    const std::string text = lubancode::runtime::FormatHostDirectoryNoticeText(
        "D:/a", "D:/b/.lubancode/worktrees/x", "model worktree enter");
    CHECK(text.find("[宿主通知]") == 0);
    CHECK(text.find("原目录: D:/a") != std::string::npos);
    CHECK(text.find("当前目录: D:/b/.lubancode/worktrees/x") != std::string::npos);
    CHECK(text.find("原因: model worktree enter") != std::string::npos);
    CHECK(text.find("后续相对路径与命令默认在当前目录执行") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 5. 通知写失败:如实报错 + 执行阻断门拦下一请求(不静默发目录过期的请求)
// ---------------------------------------------------------------------------
TEST_CASE("v3 通知写失败: 报错不落半截账,阻断门拦住 prepared") {
    EnvGuard v3gate("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("failclosed");

    std::atomic<bool> armed{false};
    auto ledger_options = LedgerOptions(root);
    ledger_options.v3_main_io_fault = [&armed]() -> std::optional<std::string> {
        if (armed.load()) return std::string("test.injected");
        return std::nullopt;
    };
    auto opened = TrajectorySessionLedger::Open(std::move(ledger_options));
    REQUIRE(opened.has_value());
    TrajectorySessionLedger& ledger = *opened;

    armed.store(true);
    const std::string error = ledger.RecordHostDirectoryNotice("D:/a", "D:/b", "model worktree enter");
    CHECK_FALSE(error.empty());
    // 账上没有半截通知:消息与接纳都不在。
    const auto stream = trajectory::v3::FindV3SessionStream(ledger.session_dir());
    REQUIRE(stream.has_value());
    CHECK(FindNoticeMessage(StreamLines(*stream)) == nullptr);

    // 调用方(controller)落账失败时置执行阻断:此后所有轮桥的 prepared
    // 一律拒发(空串即"记不住不发模型"),不带着过期目录静默发请求。
    ledger.BlockV3Execution("cwd_notice.commit_failed");
    CHECK(ledger.V3ExecutionBlocked());
    auto turn = ledger.NewTurnBridge(TrajectoryTurnBridge::Identity{"kimi", "responses", "terminal"});
    REQUIRE(turn != nullptr);
    turn->BeginTurn("turn-1", "external_user");
    turn->RecordInput(UserMessage("继续"));
    api::Request request;
    request.model = "kimi-k2.6";
    request.system = "你是 LubanCode,读写跑都走工具。";
    request.messages.push_back(UserMessage("继续"));
    CHECK(turn->OnRequestPrepared(request, agent::RequestPreparedContext{}).empty());
}
