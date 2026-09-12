// Resume 接入 v3 单 R2:session_index 对 v3 场的投影合同(可重建索引的
// 派生面)。钉五件事:
//   - session.started 带 launchCwd/runKind(新档):cwd/run_kind 的权威来源;
//   - 老档缺键:run_kind 读作"未知"(run_kind_unknown),不暗填 main_session,
//     也不被 exclude_one_shot 误伤;
//   - first_user_text 覆盖 string 与 blocks 文本块两种写法;
//   - 标题折 session.title.applied(写侧合同在 T11-A,读面先认事件);
//   - 索引版本只认当前版:旧版本(v1)缓存整份重建,未变化的主账也吃到
//     新摘要——升级投影不靠"改一字节"触发。
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "platform/paths.hpp"
#include "trajectory/session_index.hpp"
#include "trajectory/session_manager.hpp"
#include "trajectory/v3/writer.hpp"

namespace platform = lubancode::platform;
using namespace lubancode::trajectory;
using lubancode::trajectory::v3::MessageDraft;
using lubancode::trajectory::v3::MessageOrigin;
using lubancode::trajectory::v3::MessagePurpose;
using lubancode::trajectory::v3::V3Writer;
using lubancode::trajectory::v3::V3WriterOptions;

namespace {

// 摘掉格式变量:本册钉 v3 读面投影,脚手架/被测场都按产品默认走。
struct EnvUnset {
    explicit EnvUnset(const char* name) : name_(name) {
#ifdef _WIN32
        _putenv((std::string(name_) + "=").c_str());
#else
        unsetenv(name_);
#endif
    }
    ~EnvUnset() {
#ifdef _WIN32
        _putenv((std::string(name_) + "=").c_str());
#else
        unsetenv(name_);
#endif
    }
    const char* name_;
};

std::filesystem::path MakeRoot(const char* tag) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("lubancode-v3-index-proj-" + std::string(tag));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root;
}

SessionManagerOptions Opts(const std::filesystem::path& root) {
    SessionManagerOptions options;
    options.workspaces_root = root / "workspaces";
    options.workspace_root = root / "ws";
    options.launch_cwd = "D:/scaffold";
    options.lubancode_version = "0.26.258-test";
    options.v3_system_content = "你是 LubanCode。";
    return options;
}

// 开一场脚手架场把 workspace 房建出来(格式随默认;本册只借房与钥匙),
// 封口退场。回 manager 供查 key。
struct Scaffold {
    std::filesystem::path root;
    std::unique_ptr<SessionManager> manager;
    std::filesystem::path sessions_dir;
    std::filesystem::path workspace_dir;
    std::string workspace_key;

    explicit Scaffold(const char* tag) : root(MakeRoot(tag)) {
        manager = std::make_unique<SessionManager>(Opts(root));
        auto* active = manager->LaunchSession().value_or(nullptr);
        REQUIRE(active != nullptr);
        sessions_dir = active->session_dir().parent_path();
        workspace_dir = sessions_dir.parent_path();
        workspace_key = manager->workspace_key();
        NullClearParticipant participant;
        REQUIRE(manager->Close({"exit"}, &participant).error_code.empty());
    }
};

// writer 造一场 v3 账落在房里:system + session.started(带会话级事实)
// + 可选 blocks 式 user 消息 + 可选 string 式 + 可选标题事件。回会话 id。
std::string PlantWriterSession(const std::filesystem::path& sessions_dir, const std::string& tail,
                               const std::string& launch_cwd, const std::string& run_kind,
                               bool blocks_user, bool string_user, bool title_event) {
    const std::string session_id = "20260912-160405-" + tail;  // 形如 …-XXXXXX
    const std::filesystem::path dir = sessions_dir / platform::Utf8ToPath(session_id);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    REQUIRE_FALSE(ec);
    V3WriterOptions options;
    options.launch_cwd = launch_cwd;
    options.run_kind = run_kind;
    auto writer = V3Writer::Start(dir / platform::Utf8ToPath(session_id + ".jsonl"), session_id,
                                  "main-0001", "你是 LubanCode。", {}, std::move(options));
    REQUIRE(writer.has_value());
    if (blocks_user) {
        // 多块写法:首块文本 + 引用块,首句预览取首块文本,图片/引用跳过。
        MessageDraft user;
        user.origin = MessageOrigin::Human;
        user.purpose = MessagePurpose::Conversation;
        user.turn_id = "turn-1";
        user.message = nlohmann::json{{"role", "user"},
                                      {"content", nlohmann::json::array({
                                                       nlohmann::json{{"type", "text"},
                                                                      {"text", "块状的首句"}},
                                                       nlohmann::json{{"type", "text"},
                                                                      {"text", "第二块"}},
                                                   })}};
        REQUIRE(writer->AppendMessage(std::move(user)).status ==
                v3::WriteReceipt::Status::Committed);
    }
    if (string_user) {
        MessageDraft user;
        user.origin = MessageOrigin::Human;
        user.purpose = MessagePurpose::Conversation;
        user.turn_id = "turn-2";
        user.message = nlohmann::json{{"role", "user"}, {"content", "字符串的首句"}};
        REQUIRE(writer->AppendMessage(std::move(user)).status ==
                v3::WriteReceipt::Status::Committed);
    }
    if (title_event) {
        v3::EventDraft applied;
        applied.kind = v3::EventKindV3::SessionTitleApplied;
        applied.title_generation_id = "titlegen-1";
        applied.payload = nlohmann::json{{"title", "正式标题一"}};
        REQUIRE(writer->AppendEvent(std::move(applied)).status ==
                v3::WriteReceipt::Status::Committed);
    }
    return session_id;
}

// 老档:手写一段无 launchCwd/runKind 的 v3 账(2026-09 之前的 writer 写的
// 形状),落在房里。回会话 id。
std::string PlantLegacyV3Session(const std::filesystem::path& sessions_dir) {
    const std::string session_id = "20260901-090000-LEGACY";
    const std::filesystem::path dir = sessions_dir / platform::Utf8ToPath(session_id);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    REQUIRE_FALSE(ec);
    // 哈希链用写者造最稳:先 Start(不带会话级事实),再把账行直接用——
    // 这里只要"账上没有 launchCwd/runKind 键",Start 默认 options 即是。
    auto writer = V3Writer::Start(dir / platform::Utf8ToPath(session_id + ".jsonl"), session_id,
                                  "main-0001", "你是 LubanCode。", {}, V3WriterOptions{});
    REQUIRE(writer.has_value());
    MessageDraft user;
    user.origin = MessageOrigin::Human;
    user.purpose = MessagePurpose::Conversation;
    user.turn_id = "turn-1";
    user.message = nlohmann::json{{"role", "user"}, {"content", "老档的一句"}};
    REQUIRE(writer->AppendMessage(std::move(user)).status == v3::WriteReceipt::Status::Committed);
    return session_id;
}

const WorkspaceSessionSummary* FindSummary(const SessionIndexPage& page, const std::string& id) {
    for (const auto& entry : page.entries) {
        if (entry.session_id == id) {
            return &entry;
        }
    }
    return nullptr;
}

// 读房里的派生索引(整份 JSON)。
nlohmann::json ReadIndexJson(const std::filesystem::path& workspace_dir) {
    std::ifstream file(workspace_dir / "indexes" / "sessions.json", std::ios::binary);
    REQUIRE(file.is_open());
    std::stringstream buffer;
    buffer << file.rdbuf();
    const auto json = nlohmann::json::parse(buffer.str(), nullptr, false);
    REQUIRE_FALSE(json.is_discarded());
    return json;
}

}  // namespace

TEST_CASE("投影: 新档 session.started 的 cwd/run_kind 是权威来源,正文认 blocks") {
    EnvUnset unset("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS");
    Scaffold scaffold("new-archive");
    const std::string id =
        PlantWriterSession(scaffold.sessions_dir, "AAABBB", "E:/实验/甲", "main_session",
                           /*blocks_user=*/true, /*string_user=*/false, /*title_event=*/true);

    SessionIndexQuery query;
    query.current_workspace_key = scaffold.workspace_key;
    const auto page = QueryWorkspaceSessions(scaffold.root / "workspaces", query);
    const WorkspaceSessionSummary* summary = FindSummary(page, id);
    REQUIRE(summary != nullptr);
    CHECK(page.diagnostic.empty());  // 健康查询不带障碍
    CHECK(summary->cwd == "E:/实验/甲");
    CHECK(summary->run_kind == "main_session");
    CHECK_FALSE(summary->run_kind_unknown);
    CHECK(summary->first_user_text == "块状的首句");  // blocks 数组取首块文本
    CHECK(summary->title == "正式标题一");            // session.title.applied 折叠
    CHECK(summary->message_count == 1);               // 一条 human user
}

TEST_CASE("投影: 老档缺 runKind 读作未知,不暗填、不被单发过滤误伤") {
    EnvUnset unset("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS");
    Scaffold scaffold("legacy-archive");
    const std::string legacy_id = PlantLegacyV3Session(scaffold.sessions_dir);
    const std::string one_shot_id = PlantWriterSession(scaffold.sessions_dir, "CCCDDD", "",
                                                       "one_shot", false, true, false);

    SessionIndexQuery query;
    query.current_workspace_key = scaffold.workspace_key;
    query.exclude_one_shot = true;  // 选择器口径
    const auto page = QueryWorkspaceSessions(scaffold.root / "workspaces", query);

    const WorkspaceSessionSummary* legacy = FindSummary(page, legacy_id);
    REQUIRE(legacy != nullptr);  // 未知不等于单发:照列,七步 resume 再验
    CHECK(legacy->run_kind.empty());
    CHECK(legacy->run_kind_unknown);
    CHECK(legacy->cwd.empty());  // 老档没写 launchCwd:目录未知,不拿别处补
    CHECK(legacy->first_user_text == "老档的一句");

    CHECK(FindSummary(page, one_shot_id) == nullptr);  // 确凿 one_shot 才排除

    // 不过滤时单发场照列(/sessions 口径,列表是事实)。
    SessionIndexQuery plain;
    plain.current_workspace_key = scaffold.workspace_key;
    const auto all = QueryWorkspaceSessions(scaffold.root / "workspaces", plain);
    REQUIRE(FindSummary(all, one_shot_id) != nullptr);
    CHECK(FindSummary(all, one_shot_id)->run_kind == "one_shot");
}

TEST_CASE("缓存: 旧版本索引整份重建,未变化主账也吃到新投影") {
    EnvUnset unset("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS");
    Scaffold scaffold("index-version");
    const std::string legacy_id = PlantLegacyV3Session(scaffold.sessions_dir);

    SessionIndexQuery query;
    query.current_workspace_key = scaffold.workspace_key;
    {  // 第一查:写回 v2 索引。
        const auto page = QueryWorkspaceSessions(scaffold.root / "workspaces", query);
        REQUIRE(FindSummary(page, legacy_id) != nullptr);
    }
    auto index_json = ReadIndexJson(scaffold.workspace_dir);
    CHECK(index_json.value("version", 0) == 2);

    // 手工把索引降回 v1(投影升级前的缓存形状:行缺 run_kind_unknown 键,
    // run_kind 空串会被旧读法回落 main_session)——不动主账字节。
    const std::filesystem::path index_path = scaffold.workspace_dir / "indexes" / "sessions.json";
    {
        nlohmann::json stale = index_json;
        stale["version"] = 1;
        for (auto& row : stale["sessions"]) {
            row.erase("run_kind_unknown");
            row["run_kind"] = "main_session";  // 旧投影把 v3 老档当 main_session
        }
        std::ofstream out(index_path, std::ios::binary | std::ios::trunc);
        out << stale.dump();
    }

    // 第二查:旧版本不认,整份重扫——老档摘要翻回"未知",主账一字未动。
    const auto page = QueryWorkspaceSessions(scaffold.root / "workspaces", query);
    const WorkspaceSessionSummary* legacy = FindSummary(page, legacy_id);
    REQUIRE(legacy != nullptr);
    CHECK(legacy->run_kind_unknown);
    CHECK(legacy->run_kind.empty());
    const auto rebuilt = ReadIndexJson(scaffold.workspace_dir);
    CHECK(rebuilt.value("version", 0) == 2);
}
