// SessionService 测试册(AppServer 接入 Session v3 第一棒:统一服务入口
// 与身份)。三块:
//   1. 开张/v3 开关两路回归——开关关 = v2 布局(main.jsonl + session.json),
//      开关开 = v3 流(首行 system,§1.2);服务只是递合同,开关在
//      SessionManager 建场时二选一(接线点 1)。
//   2. 输入接纳与幂等(§4.2)——同键同载荷回原回执(重发只接纳一次)、
//      同键异载荷 operation_conflict、先账后回执(回执到手时 operations
//      .jsonl 已有该笔)、resume 沿来源链识别原键(新 sessionId 不洗掉
//      旧意图)。
//   3. 三端同路对照——同一操作走 CLI 路(one-shot 的开张折算
//      BuildOneShotSessionRequest)与服务路(app-server 形状的请求),
//      落账一致(同一布局合同、同一 operation 记录形状、同源 payload
//      hash)。
// json 缺键断言一律 contains()(nlohmann UB 纪律);路径断言用
// equivalent/字符串比较,不比盘符大小写。
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "app/one_shot.hpp"
#include "config/config.hpp"
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
                     ("lubancode-session-service-" + std::string(tag));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

// 服务开张请求:身份/账根钉在临时树里(免走真 cwd 裁决,测试确定性)。
runtime::SessionLaunchRequest LaunchRequestOf(const std::filesystem::path& root) {
    runtime::SessionLaunchRequest request;
    request.lubancode_version = "0.26.238-test";
    request.workspaces_root = root / "workspaces";
    request.workspace_identity = workspace::MakeFallbackIdentity(root / "ws");
    request.cwd_utf8 = tools::PathToUtf8(root / "ws");
    return request;
}

std::vector<nlohmann::json> ReadJsonl(const std::filesystem::path& path) {
    std::vector<nlohmann::json> lines;
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return lines;
    }
    std::string text;
    while (std::getline(in, text)) {
        if (text.empty()) {
            continue;
        }
        lines.push_back(nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false));
    }
    return lines;
}

std::filesystem::path V3StreamOf(const std::filesystem::path& session_dir) {
    return session_dir / (tools::PathToUtf8(session_dir.filename()) + ".jsonl");
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. 开张与 v3 开关两路回归(§十.1"创建会话接 v3 写侧开关")
// ---------------------------------------------------------------------------

TEST_CASE("服务开张:开关关走 v2 布局,v3 流一枚不长") {
    const auto root = FreshRoot("launch-v2");
    runtime::SessionService service(LaunchRequestOf(root));
    REQUIRE(service.runtime() != nullptr);
    REQUIRE(service.trajectory() != nullptr);
    CHECK(service.launch_error().empty());
    CHECK_FALSE(service.v3_format());

    const std::filesystem::path session_dir = service.trajectory()->session_dir();
    CHECK(std::filesystem::exists(session_dir / "main.jsonl"));
    CHECK(std::filesystem::exists(session_dir / "session.json"));
    CHECK_FALSE(std::filesystem::exists(V3StreamOf(session_dir)));
    CHECK_FALSE(service.trajectory()->session_id().empty());

    // 台账文件随首笔接纳出现,不在开张时空造。
    CHECK_FALSE(std::filesystem::exists(session_dir / "operations.jsonl"));
    const auto closed = service.Close("exit");
    CHECK(closed.error_code.empty());
}

TEST_CASE("服务开张:开关开走 v3 流,首行 system(§1.2),v2 文件一枚不长") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("launch-v3");
    runtime::SessionService service(LaunchRequestOf(root));
    REQUIRE(service.runtime() != nullptr);
    REQUIRE(service.trajectory() != nullptr);
    CHECK(service.v3_format());

    const std::filesystem::path session_dir = service.trajectory()->session_dir();
    const std::filesystem::path stream = V3StreamOf(session_dir);
    REQUIRE(std::filesystem::exists(stream));
    CHECK_FALSE(std::filesystem::exists(session_dir / "main.jsonl"));
    CHECK_FALSE(std::filesystem::exists(session_dir / "session.json"));

    const auto rows = ReadJsonl(stream);
    REQUIRE(rows.size() >= 2);
    REQUIRE(rows[0].contains("schemaVersion"));
    CHECK(rows[0]["schemaVersion"] == 3);
    CHECK(rows[0].value("type", std::string()) == "message");
    CHECK(rows[0].contains("turnId"));
    CHECK(rows[0]["turnId"].is_null());
    CHECK(rows[0].contains("message"));
    CHECK(rows[0]["message"].value("role", std::string()) == "system");
    CHECK(rows[0].value("sessionId", std::string()) == service.trajectory()->session_id());

    const auto closed = service.Close("exit");
    CHECK(closed.error_code.empty());
}

// ---------------------------------------------------------------------------
// 2. 输入接纳:幂等(§4.2)与先账后回执
// ---------------------------------------------------------------------------

TEST_CASE("输入接纳幂等:同键同载荷回原回执,同键异载荷冲突,重发只接纳一次") {
    const auto root = FreshRoot("input-idempotent");
    runtime::SessionService service(LaunchRequestOf(root));

    runtime::SessionService::InputRequest first;
    first.client_operation_id = "OP-71";
    first.text = "继续检查恢复路径";
    const auto r1 = service.SubmitInput(first);
    CHECK(r1.accepted);
    CHECK_FALSE(r1.duplicate);
    CHECK(r1.error_code.empty());
    CHECK(r1.operation_id == "op-1");
    CHECK(r1.input_id == "in-1");
    CHECK_FALSE(r1.payload_hash.empty());
    CHECK(service.pending_input_count() == 1);

    // 同键同载荷重发:原回执,不重复接纳(队列不涨)。
    const auto r2 = service.SubmitInput(first);
    CHECK_FALSE(r2.accepted);
    CHECK(r2.duplicate);
    CHECK(r2.error_code.empty());
    CHECK(r2.operation_id == r1.operation_id);
    CHECK(r2.input_id == r1.input_id);
    CHECK(r2.payload_hash == r1.payload_hash);
    CHECK(service.pending_input_count() == 1);

    // 同键不同载荷:operation_conflict,不入队。
    runtime::SessionService::InputRequest clash = first;
    clash.text = "换个正文";
    const auto r3 = service.SubmitInput(clash);
    CHECK_FALSE(r3.accepted);
    CHECK_FALSE(r3.duplicate);
    CHECK(r3.error_code == "operation_conflict");
    CHECK(service.pending_input_count() == 1);

    // 无键:每发必纳(协议 1.2 面没有 clientOperationId 的那条路)。
    runtime::SessionService::InputRequest keyless;
    keyless.text = "无键输入";
    CHECK(service.SubmitInput(keyless).accepted);
    CHECK(service.SubmitInput(keyless).accepted);
    CHECK(service.pending_input_count() == 3);

    // 泵侧消费:FIFO,带接纳时的操作号。
    const auto popped = service.PopPendingInput();
    REQUIRE(popped.has_value());
    CHECK(popped->text == "继续检查恢复路径");
    CHECK(popped->operation_id == r1.operation_id);
    CHECK(service.PopPendingInput().has_value());
    CHECK(service.PopPendingInput().has_value());
    CHECK_FALSE(service.PopPendingInput().has_value());
    CHECK(service.pending_input_count() == 0);
}

TEST_CASE("先账后回执:回执到手时操作事实已落盘(§4.2 崩溃窗口)") {
    const auto root = FreshRoot("account-before-receipt");
    runtime::SessionService service(LaunchRequestOf(root));

    runtime::SessionService::InputRequest input;
    input.client_operation_id = "OP-RECEIPT";
    input.text = "问一句";
    const auto receipt = service.SubmitInput(input);
    REQUIRE(receipt.accepted);

    // 回执返回后立刻读盘:该笔 operation.accepted 已在(先账),带键与
    // payload hash——崩溃在回执送达前发生,重发也能按台账命中。
    const std::filesystem::path operations =
        service.trajectory()->session_dir() / "operations.jsonl";
    const auto lines = ReadJsonl(operations);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].value("kind", std::string()) == "operation.accepted");
    CHECK(lines[0].value("operationId", std::string()) == receipt.operation_id);
    CHECK(lines[0].value("inputId", std::string()) == receipt.input_id);
    CHECK(lines[0].value("clientOperationId", std::string()) == "OP-RECEIPT");
    CHECK(lines[0].value("payloadHash", std::string()) == receipt.payload_hash);
    CHECK(lines[0].contains("receivedAtMs"));
}

TEST_CASE("载荷 hash 同源:同一正文与图片,两条服务路算出同一 hash") {
    const auto root_a = FreshRoot("hash-a");
    const auto root_b = FreshRoot("hash-b");
    runtime::SessionService a(LaunchRequestOf(root_a));
    runtime::SessionService b(LaunchRequestOf(root_b));

    runtime::SessionService::InputRequest input;
    input.text = "看这张图";
    api::ImageBlock block;
    block.media_type = "image/png";
    block.data = "aGk=";
    block.filename = "shot.png";
    block.width = 640;
    block.height = 480;
    input.images.push_back(block);

    const auto ra = a.SubmitInput(input);
    const auto rb = b.SubmitInput(input);
    REQUIRE(ra.accepted);
    REQUIRE(rb.accepted);
    CHECK(ra.payload_hash == rb.payload_hash);
    // 与公开折算规则对得上(对照测试的同源依据)。
    CHECK(ra.payload_hash == platform::Sha256Hex(runtime::SessionService::CanonicalInputPayload(input)));
}

// ---------------------------------------------------------------------------
// 3. 恢复:resume-at-launch(ResumeAsNew 两路分派在账本侧,服务递合同)
// ---------------------------------------------------------------------------

TEST_CASE("恢复:v2 源 resume-at-launch 开新段,来源可查(§10.4)") {
    const auto root = FreshRoot("resume-v2");
    std::string source_id;
    {
        runtime::SessionService source(LaunchRequestOf(root));
        REQUIRE(source.trajectory() != nullptr);
        source_id = source.trajectory()->session_id();
        const auto closed = source.Close("exit");
        CHECK(closed.error_code.empty());
    }
    runtime::SessionLaunchRequest resume_request = LaunchRequestOf(root);
    resume_request.resume_at_launch = true;
    runtime::SessionService resumed(resume_request);
    REQUIRE(resumed.trajectory() != nullptr);
    CHECK(resumed.runtime()->trajectory()->resumed_at_launch());
    CHECK(resumed.trajectory()->session_id() != source_id);
    CHECK(resumed.trajectory()->session_id() == resumed.runtime()->trajectory()->session_id());
    // 直接来源可查(操作台账种账的钥匙)。
    CHECK(resumed.runtime()->trajectory()->launch_resume_source_session_id() == source_id);
    CHECK_FALSE(resumed.v3_format());

    const auto closed = resumed.Close("exit");
    CHECK(closed.error_code.empty());
}

TEST_CASE("恢复:v3 源 + 开关开,新段同为 v3(两路分派的 v3 侧)") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("resume-v3");
    std::string source_id;
    {
        runtime::SessionService source(LaunchRequestOf(root));
        REQUIRE(source.trajectory() != nullptr);
        REQUIRE(source.v3_format());
        source_id = source.trajectory()->session_id();
        const auto closed = source.Close("exit");
        CHECK(closed.error_code.empty());
    }
    runtime::SessionLaunchRequest resume_request = LaunchRequestOf(root);
    resume_request.resume_at_launch = true;
    runtime::SessionService resumed(resume_request);
    REQUIRE(resumed.trajectory() != nullptr);
    CHECK(resumed.runtime()->trajectory()->resumed_at_launch());
    CHECK(resumed.trajectory()->session_id() != source_id);
    CHECK(resumed.v3_format());
    CHECK(std::filesystem::exists(V3StreamOf(resumed.trajectory()->session_dir())));
    CHECK_FALSE(std::filesystem::exists(resumed.trajectory()->session_dir() / "main.jsonl"));

    const auto closed = resumed.Close("exit");
    CHECK(closed.error_code.empty());
}

TEST_CASE("恢复沿来源链识别原键:来源场的操作台账种进新场,旧意图不重复执行") {
    const auto root = FreshRoot("resume-dedup");
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
    // 来源场收不到回执的崩溃窗口:意图已落账,回执没送达。
    runtime::SessionService::InputRequest lost;
    lost.client_operation_id = "OP-CRASH";
    lost.text = "崩溃前发出的那句话";
    const std::string lost_hash =
        platform::Sha256Hex(runtime::SessionService::CanonicalInputPayload(lost));
    {
        nlohmann::json line{{"schemaVersion", 1},
                            {"kind", "operation.accepted"},
                            {"operationId", "op-9"},
                            {"inputId", "in-9"},
                            {"clientOperationId", lost.client_operation_id},
                            {"payloadHash", lost_hash},
                            {"receivedAtMs", 1757500000000}};
        std::ofstream out(source_dir / "operations.jsonl", std::ios::app | std::ios::binary);
        out << line.dump() << "\n";
    }
    // resume-as-new 开新场:新 sessionId 不洗掉旧意图——同键同载荷给原回执
    //(op-9),不同载荷给冲突,都不重复接纳。
    runtime::SessionLaunchRequest resume_request = LaunchRequestOf(root);
    resume_request.resume_at_launch = true;
    runtime::SessionService resumed(resume_request);
    REQUIRE(resumed.runtime()->trajectory()->resumed_at_launch());

    const auto r1 = resumed.SubmitInput(lost);
    CHECK_FALSE(r1.accepted);
    CHECK(r1.duplicate);
    CHECK(r1.operation_id == "op-9");
    CHECK(r1.input_id == "in-9");
    CHECK(resumed.pending_input_count() == 0);  // 没有为旧意图再排一次队

    runtime::SessionService::InputRequest different = lost;
    different.text = "换了正文";
    const auto r2 = resumed.SubmitInput(different);
    CHECK_FALSE(r2.accepted);
    CHECK(r2.error_code == "operation_conflict");
    CHECK(resumed.pending_input_count() == 0);
}

// ---------------------------------------------------------------------------
// 4. 三端同路对照:同一操作走 CLI 路(one-shot 折算)与服务路,落账一致
// ---------------------------------------------------------------------------

TEST_CASE("三端同路:CLI 路(one-shot 折算)与服务路,开张/接纳/收口落账一致") {
    const auto cli_root = FreshRoot("same-road-cli");
    const auto service_root = FreshRoot("same-road-service");

    // CLI 路:单发入口的开张折算(BuildOneShotSessionRequest),账根钉临时树。
    const lubancode::config::Config config;
    runtime::SessionLaunchRequest cli_request =
        lubancode::app::BuildOneShotSessionRequest(config, tools::PathToUtf8(cli_root / "ws"));
    // 折算料位先钉死:单发场单列(与 AskOnce 行为同源)。
    CHECK(cli_request.one_shot);
    CHECK(cli_request.launch_cwd == tools::PathToUtf8(cli_root / "ws"));
    CHECK(cli_request.training_policy == trajectory::TrainingPolicy::Exclude);
    cli_request.workspaces_root = cli_root / "workspaces";
    cli_request.workspace_identity = workspace::MakeFallbackIdentity(cli_root / "ws");
    runtime::SessionService cli_service(cli_request);
    REQUIRE(cli_service.trajectory() != nullptr);

    // 服务路:app-server 形状(HandleThreadStart 递的料:cwd + 版本 + 账根)。
    runtime::SessionLaunchRequest server_request;
    server_request.cwd_utf8 = tools::PathToUtf8(service_root / "ws");
    server_request.lubancode_version = "0.26.238-test";
    server_request.workspaces_root = service_root / "workspaces";
    server_request.workspace_identity = workspace::MakeFallbackIdentity(service_root / "ws");
    runtime::SessionService server_service(server_request);
    REQUIRE(server_service.trajectory() != nullptr);

    // 同一操作(同键同载荷)各走一路:接纳回执同源(同 payload hash、同
    // 记录形状),台账逐字段一致(除身份与时间)。
    runtime::SessionService::InputRequest input;
    input.client_operation_id = "OP-SAME-ROAD";
    input.text = "同一句话,两条路";
    const auto cli_receipt = cli_service.SubmitInput(input);
    const auto server_receipt = server_service.SubmitInput(input);
    REQUIRE(cli_receipt.accepted);
    REQUIRE(server_receipt.accepted);
    CHECK(cli_receipt.payload_hash == server_receipt.payload_hash);
    CHECK(cli_receipt.operation_id == server_receipt.operation_id);

    const auto cli_lines =
        ReadJsonl(cli_service.trajectory()->session_dir() / "operations.jsonl");
    const auto server_lines =
        ReadJsonl(server_service.trajectory()->session_dir() / "operations.jsonl");
    REQUIRE(cli_lines.size() == 1);
    REQUIRE(server_lines.size() == 1);
    CHECK(cli_lines[0].value("kind", std::string()) ==
          server_lines[0].value("kind", std::string()));
    CHECK(cli_lines[0].value("operationId", std::string()) ==
          server_lines[0].value("operationId", std::string()));
    CHECK(cli_lines[0].value("clientOperationId", std::string()) ==
          server_lines[0].value("clientOperationId", std::string()));
    CHECK(cli_lines[0].value("payloadHash", std::string()) ==
          server_lines[0].value("payloadHash", std::string()));

    // 布局合同同款:都是 v2 场(main.jsonl + session.json 在,v3 流不在)。
    for (const runtime::SessionService* service : {&cli_service, &server_service}) {
        const std::filesystem::path session_dir = service->trajectory()->session_dir();
        CHECK(std::filesystem::exists(session_dir / "main.jsonl"));
        CHECK(std::filesystem::exists(session_dir / "session.json"));
        CHECK_FALSE(std::filesystem::exists(V3StreamOf(session_dir)));
    }

    // 幂等同规矩:重发一次只接纳一次,两路都如此。
    const auto cli_again = cli_service.SubmitInput(input);
    const auto server_again = server_service.SubmitInput(input);
    CHECK(cli_again.duplicate);
    CHECK(server_again.duplicate);
    CHECK(cli_service.pending_input_count() == 1);
    CHECK(server_service.pending_input_count() == 1);

    // 收口同口:两路 reason 各按现行口径,封完 session.json 报 closed。
    const auto cli_closed = cli_service.Close("exit");
    const auto server_closed = server_service.Close("thread_stop");
    CHECK(cli_closed.error_code.empty());
    CHECK(server_closed.error_code.empty());
    for (const std::filesystem::path& root : {cli_root, service_root}) {
        bool found_closed = false;
        std::error_code walk_ec;
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(root / "workspaces", walk_ec)) {
            if (entry.is_regular_file() && entry.path().filename() == "session.json") {
                std::ifstream in(entry.path(), std::ios::binary);
                std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                const nlohmann::json manifest =
                    nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
                if (manifest.is_object() && manifest.contains("status") &&
                    manifest["status"] == "closed") {
                    found_closed = true;
                }
            }
        }
        CHECK(found_closed);
    }
}
