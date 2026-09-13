// 常驻总装 V0 第三/第五件事的受理册:SessionService 受理四病(账行失败
// 仍成功、正文只在内存、pending 不重建、单跳来源去重)修复后的持久底线,
// 与放行门的可注入窗口。真拔电/真杀进程属后续批次真机验收——本册用
// "账态注入 + 重开服务"模拟崩溃后的重建面,不冒充进程级硬杀证据(如实
// 分账)。
//
//   1. 受理次序:原件先落稳(operations-inputs/<op>.json) → accepted 行
//      (schemaVersion=2,带 inputRef)落稳 → 回执;正文含图片整份可恢复。
//   2. 放行门·accepted 落稳、内存入队前"硬杀"(注入:提交后不消费即弃
//      服务):resume 沿链重建 pending,正文与键都在,不丢不双。
//   3. 放行门·原件已落、accepted 未落(注入孤立原件):重开不重排、无账
//      可依,孤立原件无害。
//   4. 放行门·dispatched 已落(claim/开轮前的可注入窗口):重开不重排
//      ——不双派发;真 claim/开轮裁决属 V1 主泵,未验。
//   5. 多跳来源链:A→B→C 三场连崩,C 一跳内拿到 A 的未派发输入;同键
//      同载荷回 duplicate(旧意图不重复执行)。
//   6. 磁盘写失败:账文件被目录占位 → operation.append_failed,不回成功,
//      后续受理持续拒绝(broken 传播 = 写盘失败停止受理);原件目录被占
//      → operation.artifact_failed。
//   7. v1 旧账行(无 inputRef)兼容:种去重表,不重排 pending。
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "platform/sha256.hpp"
#include "runtime/session_service.hpp"
#include "tools/path_utils.hpp"
#include "workspace/identity.hpp"

using namespace lubancode;

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

std::filesystem::path FreshRoot(const char* tag) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lubancode-session-durability-" + std::string(tag));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

runtime::SessionLaunchRequest LaunchRequestOf(const std::filesystem::path& root) {
    runtime::SessionLaunchRequest request;
    request.lubancode_version = "0.26.238-test";
    request.workspaces_root = root / "workspaces";
    request.workspace_identity = workspace::MakeFallbackIdentity(root / "ws");
    request.cwd_utf8 = tools::PathToUtf8(root / "ws");
    return request;
}

runtime::SessionLaunchRequest ResumeRequestOf(const std::filesystem::path& root,
                                              const std::string& source_id) {
    auto request = LaunchRequestOf(root);
    request.resume_at_launch = true;
    request.resume_source_session_id = source_id;
    return request;
}

std::vector<nlohmann::json> ReadJsonl(const std::filesystem::path& path) {
    std::vector<nlohmann::json> lines;
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return lines;
    std::string text;
    while (std::getline(in, text)) {
        if (text.empty()) continue;
        lines.push_back(nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false));
    }
    return lines;
}

void AppendJsonl(const std::filesystem::path& path, const nlohmann::json& line) {
    std::ofstream out(path, std::ios::app | std::ios::binary);
    out << line.dump() << "\n";
}

runtime::SessionService::InputRequest InputOf(const std::string& key, const std::string& text) {
    runtime::SessionService::InputRequest input;
    input.client_operation_id = key;
    input.text = text;
    return input;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. 受理次序:原件先落稳、账行带 inputRef、正文整份可恢复
// ---------------------------------------------------------------------------

TEST_CASE("受理落稳:原件与账行都在,正文含图片整份可读回") {
    EnvGuard v2pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "0");
    const auto root = FreshRoot("artifact");
    runtime::SessionService service(LaunchRequestOf(root));
    REQUIRE(service.trajectory() != nullptr);
    const std::filesystem::path session_dir = service.trajectory()->session_dir();

    runtime::SessionService::InputRequest input = InputOf("OP-A1", "带图问一句");
    api::ImageBlock block;
    block.media_type = "image/png";
    block.data = "aGk=";
    block.filename = "shot.png";
    block.width = 320;
    block.height = 240;
    input.images.push_back(block);

    const auto receipt = service.SubmitInput(input);
    REQUIRE(receipt.accepted);
    CHECK(receipt.error_code.empty());

    // 原件:先于回执落稳(回执到手即在盘上)。
    const std::filesystem::path artifact =
        session_dir / "operations-inputs" / (receipt.operation_id + ".json");
    REQUIRE(std::filesystem::exists(artifact));
    {
        std::ifstream in(artifact, std::ios::binary);
        const nlohmann::json stored = nlohmann::json::parse(
            std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()),
            nullptr, false);
        REQUIRE(stored.is_object());
        CHECK(stored.value("text", std::string()) == "带图问一句");
        REQUIRE(stored.contains("images") && stored["images"].is_array() &&
                stored["images"].size() == 1);
        CHECK(stored["images"][0].value("data", std::string()) == "aGk=");
        CHECK(stored["images"][0].value("width", 0) == 320);
    }
    // 账行:schemaVersion=2,带 inputRef 指原件。
    const auto lines = ReadJsonl(session_dir / "operations.jsonl");
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].value("kind", std::string()) == "operation.accepted");
    CHECK(lines[0].value("schemaVersion", 0) == 2);
    CHECK(lines[0].value("inputRef", std::string()) ==
          "operations-inputs/" + receipt.operation_id + ".json");
    CHECK(lines[0].value("payloadHash", std::string()) == receipt.payload_hash);
}

// ---------------------------------------------------------------------------
// 2. 放行门:accepted 落稳、内存入队前崩 → 重开重建,不丢不双
// ---------------------------------------------------------------------------

TEST_CASE("崩溃窗口重建:未派发输入沿 resume 链重排进 pending,正文与键都在") {
    EnvGuard v2pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "0");
    const auto root = FreshRoot("revive");
    std::string source_id;
    std::filesystem::path source_dir;
    {
        runtime::SessionService source(LaunchRequestOf(root));
        REQUIRE(source.trajectory() != nullptr);
        source_id = source.trajectory()->session_id();
        source_dir = source.trajectory()->session_dir();
        // 两笔受理,一笔带键一笔不带;都不消费(硬杀窗口:账已落稳,
        // pending 只在内存)。
        REQUIRE(source.SubmitInput(InputOf("OP-K1", "第一句要接住")).accepted);
        REQUIRE(source.SubmitInput(InputOf("", "无键的也接住")).accepted);
        REQUIRE(source.pending_input_count() == 2);
        // 不 Close、不 Pop:直接弃场(析构不封口,锁随 RAII 释放)。
    }
    REQUIRE(std::filesystem::exists(source_dir / "operations.jsonl"));

    runtime::SessionService resumed(ResumeRequestOf(root, source_id));
    REQUIRE(resumed.trajectory() != nullptr);
    // 重建:两笔都回来,正文完整、次序 FIFO。
    REQUIRE(resumed.pending_input_count() == 2);
    auto first = resumed.PopPendingInput();
    REQUIRE(first.status == runtime::SessionService::PendingPop::Status::Ok);
    CHECK(first.input.text == "第一句要接住");
    auto second = resumed.PopPendingInput();
    REQUIRE(second.status == runtime::SessionService::PendingPop::Status::Ok);
    CHECK(second.input.text == "无键的也接住");
    auto drained = resumed.PopPendingInput();
    CHECK(drained.status == runtime::SessionService::PendingPop::Status::Empty);

    // 重落在账:新场的 operations.jsonl 有两笔带 origin 审计的 accepted
    // 行,指向来源场与原操作号。
    const auto lines = ReadJsonl(resumed.trajectory()->session_dir() / "operations.jsonl");
    REQUIRE(lines.size() == 4);  // 2 accepted(重落) + 2 dispatched(Pop 落账)
    int relocated = 0;
    for (const auto& line : lines) {
        if (line.value("kind", std::string()) == "operation.accepted" &&
            line.contains("originSessionId") && line.value("originSessionId", std::string()) == source_id) {
            ++relocated;
            CHECK(line.contains("originOperationId"));
            CHECK(line.value("inputRef", std::string()).find("operations-inputs/") == 0);
        }
    }
    CHECK(relocated == 2);

    // 同键重发:回 duplicate(原意图不重复执行),队列不涨。
    const auto again = resumed.SubmitInput(InputOf("OP-K1", "第一句要接住"));
    CHECK_FALSE(again.accepted);
    CHECK(again.duplicate);
    CHECK(resumed.pending_input_count() == 0);
}

// ---------------------------------------------------------------------------
// 3. 放行门:原件已落、accepted 未落 → 孤立原件,无账不重排
// ---------------------------------------------------------------------------

TEST_CASE("孤立原件:无 accepted 账行驱动,重开不重排") {
    EnvGuard v2pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "0");
    const auto root = FreshRoot("orphan");
    std::string source_id;
    std::filesystem::path source_dir;
    {
        runtime::SessionService source(LaunchRequestOf(root));
        REQUIRE(source.trajectory() != nullptr);
        source_id = source.trajectory()->session_id();
        source_dir = source.trajectory()->session_dir();
        // 受理一笔并正常派发(dispatched 已落),再注入一枚"账没落上"的
        // 孤立原件——模拟 accepted 行写失败留下的残留。
        REQUIRE(source.SubmitInput(InputOf("OP-D1", "已派发的那句")).accepted);
        REQUIRE(source.PopPendingInput().status ==
                runtime::SessionService::PendingPop::Status::Ok);
        const std::filesystem::path orphan =
            source_dir / "operations-inputs" / "op-9.json";
        std::error_code ec;
        std::filesystem::create_directories(orphan.parent_path(), ec);
        std::ofstream out(orphan, std::ios::binary | std::ios::trunc);
        out << R"({"schemaVersion":1,"operationId":"op-9","text":"没账的正文","images":[]})";
    }

    runtime::SessionService resumed(ResumeRequestOf(root, source_id));
    REQUIRE(resumed.trajectory() != nullptr);
    // 已派发的不重排;孤立原件无账行驱动,同样不进队列(可回收,不冒充受理)。
    CHECK(resumed.pending_input_count() == 0);
    // 去重表照旧:同键同载荷 duplicate,异载荷 conflict。
    CHECK(resumed.SubmitInput(InputOf("OP-D1", "已派发的那句")).duplicate);
    auto clash = resumed.SubmitInput(InputOf("OP-D1", "换了正文"));
    CHECK(clash.error_code == "operation_conflict");
}

// ---------------------------------------------------------------------------
// 4. 放行门:dispatched 已落(claim/开轮前窗口的可注入面)
// ---------------------------------------------------------------------------

TEST_CASE("已派发不重排:dispatched 行挡住重复派发") {
    EnvGuard v2pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "0");
    const auto root = FreshRoot("dispatched");
    std::string source_id;
    {
        runtime::SessionService source(LaunchRequestOf(root));
        REQUIRE(source.trajectory() != nullptr);
        source_id = source.trajectory()->session_id();
        REQUIRE(source.SubmitInput(InputOf("OP-C1", "派发后崩的那句")).accepted);
        // 泵侧取走(dispatched 落稳)后崩——重开不得再排同一笔。
        REQUIRE(source.PopPendingInput().status ==
                runtime::SessionService::PendingPop::Status::Ok);
        CHECK(source.pending_input_count() == 0);
    }
    runtime::SessionService resumed(ResumeRequestOf(root, source_id));
    REQUIRE(resumed.trajectory() != nullptr);
    CHECK(resumed.pending_input_count() == 0);  // 无双派发
    // 真实 claim/开轮窗口的裁决(核实没有旧执行再开轮)属 V1 主泵:未验。
}

// ---------------------------------------------------------------------------
// 5. 多跳来源链:连崩两场,去重与重建穿全链
// ---------------------------------------------------------------------------

TEST_CASE("多跳来源链:A 崩 B 接 B 又崩 C 接,同一意图只一份、原键全链可辨") {
    EnvGuard v2pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "0");
    const auto root = FreshRoot("chain");
    std::string id_a;
    std::string id_b;
    {
        runtime::SessionService a(LaunchRequestOf(root));
        REQUIRE(a.trajectory() != nullptr);
        id_a = a.trajectory()->session_id();
        REQUIRE(a.SubmitInput(InputOf("OP-M1", "跨三场的那句话")).accepted);
    }
    {
        runtime::SessionService b(ResumeRequestOf(root, id_a));
        REQUIRE(b.trajectory() != nullptr);
        id_b = b.trajectory()->session_id();
        CHECK(b.pending_input_count() == 1);  // A 的未派发输入重排进 B
        // B 也崩(没消费)。
    }
    runtime::SessionService c(ResumeRequestOf(root, id_b));
    REQUIRE(c.trajectory() != nullptr);
    // C 一跳拿到(重落后 B 的账自足),只此一份。
    REQUIRE(c.pending_input_count() == 1);
    auto revived = c.PopPendingInput();
    REQUIRE(revived.status == runtime::SessionService::PendingPop::Status::Ok);
    CHECK(revived.input.text == "跨三场的那句话");
    CHECK(c.pending_input_count() == 0);
    // 同键同载荷:duplicate——首发的原回执语义穿全链。
    auto again = c.SubmitInput(InputOf("OP-M1", "跨三场的那句话"));
    CHECK_FALSE(again.accepted);
    CHECK(again.duplicate);
    CHECK(again.payload_hash == platform::Sha256Hex(
                                   runtime::SessionService::CanonicalInputPayload(
                                       InputOf("OP-M1", "跨三场的那句话"))));
}

// ---------------------------------------------------------------------------
// 6. 磁盘写失败:不回成功回执,受理面停住
// ---------------------------------------------------------------------------

TEST_CASE("账文件写不进:拒受理不回成功,broken 传播停住后续受理") {
    EnvGuard v2pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "0");
    const auto root = FreshRoot("diskfull");
    runtime::SessionService service(LaunchRequestOf(root));
    REQUIRE(service.trajectory() != nullptr);
    const std::filesystem::path operations =
        service.trajectory()->session_dir() / "operations.jsonl";
    std::error_code ec;
    std::filesystem::create_directories(operations, ec);  // 目录占位:开账必失败
    REQUIRE(std::filesystem::is_directory(operations));

    const auto receipt = service.SubmitInput(InputOf("OP-W1", "写不进盘的这句"));
    CHECK_FALSE(receipt.accepted);
    CHECK(receipt.error_code == "operation.append_failed");
    CHECK(service.pending_input_count() == 0);

    // broken 传播:第二笔同样拒(写盘失败停止受理,不是退化为内存受理)。
    const auto second = service.SubmitInput(InputOf("OP-W2", "再试一句"));
    CHECK_FALSE(second.accepted);
    CHECK(second.error_code == "operation.append_failed");
    CHECK(service.pending_input_count() == 0);
}

TEST_CASE("原件落不稳:同样拒受理,不回成功") {
    EnvGuard v2pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "0");
    const auto root = FreshRoot("artifactfail");
    runtime::SessionService service(LaunchRequestOf(root));
    REQUIRE(service.trajectory() != nullptr);
    const std::filesystem::path artifacts =
        service.trajectory()->session_dir() / "operations-inputs";
    std::error_code ec;
    // 目录位被一个同名"文件"占着:建不出原件目录。
    std::filesystem::create_directories(artifacts.parent_path(), ec);
    std::ofstream blocker(artifacts, std::ios::binary | std::ios::trunc);
    blocker << "not a directory";
    blocker.close();
    REQUIRE(std::filesystem::is_regular_file(artifacts));

    const auto receipt = service.SubmitInput(InputOf("OP-W3", "原件写不进的这句"));
    CHECK_FALSE(receipt.accepted);
    CHECK(receipt.error_code == "operation.artifact_failed");
    CHECK(service.pending_input_count() == 0);
    // 账上一行没落(先账后回执:原件失败连账行都不写)。
    CHECK_FALSE(std::filesystem::exists(service.trajectory()->session_dir() /
                                        "operations.jsonl"));
}

// ---------------------------------------------------------------------------
// 7. v1 旧账行兼容:无原件不重排,去重照旧
// ---------------------------------------------------------------------------

TEST_CASE("v1 旧账行:种去重表,不重排 pending(正文不可恢复,如实降级)") {
    EnvGuard v2pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "0");
    const auto root = FreshRoot("legacy-v1");
    std::string source_id;
    std::filesystem::path source_dir;
    {
        runtime::SessionService source(LaunchRequestOf(root));
        REQUIRE(source.trajectory() != nullptr);
        source_id = source.trajectory()->session_id();
        source_dir = source.trajectory()->session_dir();
        const auto closed = source.Close("exit");
        CHECK(closed.error_code.empty());
    }
    // 手工注入 v1 形状的账行(schemaVersion=1,无 inputRef)。
    AppendJsonl(source_dir / "operations.jsonl",
                nlohmann::json{{"schemaVersion", 1},
                               {"kind", "operation.accepted"},
                               {"operationId", "op-1"},
                               {"inputId", "in-1"},
                               {"clientOperationId", "OP-OLD"},
                               {"payloadHash", platform::Sha256Hex("旧正文")},
                               {"receivedAtMs", 1757500000000}});

    runtime::SessionService resumed(ResumeRequestOf(root, source_id));
    REQUIRE(resumed.trajectory() != nullptr);
    CHECK(resumed.pending_input_count() == 0);  // 无原件:不凭空造正文
    const auto again = resumed.SubmitInput(InputOf("OP-OLD", "旧正文"));
    CHECK(again.duplicate);  // 去重表照种
    CHECK(again.operation_id == "op-1");
}
