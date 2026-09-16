// v3 结果原文追回测试(T17/V3-ADD-03):超预览预算的工具结果,原文按
// v3 ResultStore 落档(artifacts/res-*),模型侧只收预算内预览——预览
// 说明区必须给"模型自己读得回"的入口:full_output/captured_output 列
// 绝对路径 + retrieval_hint 指路 read_file 分段读。生产链(提交边界
// V3ToolResultsCommitted 与降档重派生)的两步在这里逐段钉:
//   Persist → PreviewFromPersistedMaterials(绝对路径拼装) → BuildToolPreview;
// 不另造第二套结果仓,也不复活的专用读取工具(旧 context_search/
// context_read 已随 ContextArtifactStore 退役)。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include "hooks/hash.hpp"  // Sha256Hex:与账上 result_ref 同一把尺
#include "platform/paths.hpp"
#include "runtime/v3_tool_result_material.hpp"
#include "trajectory/v3/result_store.hpp"

using namespace lubancode::trajectory::v3;
using lubancode::runtime::PreviewFromPersistedMaterials;

namespace {

std::string ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

struct RetrievalHarness {
    std::filesystem::path session_dir;

    explicit RetrievalHarness(const char* tag) {
        session_dir = std::filesystem::temp_directory_path() /
                      ("lubancode-v3-retrieval-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(session_dir, ec);
        std::filesystem::create_directories(session_dir, ec);
    }

    std::expected<ResultStore, std::string> Open() { return ResultStore::Open(session_dir); }
};

// 一份超预算的 combined 输出:头/中/尾三段各带可核对的证据串。
std::string LongCombinedOutput() {
    std::string head = "[头部] build start\n";
    for (int i = 0; i < 400; ++i) {
        head += "filler head line " + std::to_string(i) + "\n";
    }
    std::string middle = "[中段证据] undefined reference to `needle_retention_marker()'\n";
    for (int i = 0; i < 400; ++i) {
        middle += "filler middle line " + std::to_string(i) + "\n";
    }
    std::string tail = "[尾部] build finished with status 2\n";
    return head + middle + tail;
}

ResultStore::PersistRequest TextPersist(const std::string& content, const std::string& action_id,
                                         const std::string& terminal_event) {
    ResultStore::PersistRequest persist;
    persist.result_kind = "text";
    persist.content = content;
    persist.execution_event_ref = terminal_event;
    persist.tool_call_id = action_id;
    persist.preview_policy = {{"policy", "v3-tool-preview"}, {"maxPreviewBytes", 4096}};
    persist.outputs.push_back(ResultStore::ChannelOutput{
        "combined", "text/plain", content, /*capture_complete=*/true, std::string(),
        static_cast<std::uint64_t>(content.size()), /*lower_bound=*/false});
    return persist;
}

}  // namespace

TEST_CASE("追回链:超预算结果的预览给绝对路径,中段证据经该路径读得回") {
    RetrievalHarness harness("absolute");
    const std::string output = LongCombinedOutput();
    auto store_or = harness.Open();
    REQUIRE(store_or.has_value());

    const auto persisted = store_or->Persist(TextPersist(output, "act-000001", "evt-9"));
    REQUIRE(persisted.ok);

    // 生产链同款两步:绝对路径由 PreviewFromPersistedMaterials 按
    // session_dir 拼(账上 result_ref.path 保持相对,可移植)。
    const auto request = PreviewFromPersistedMaterials(
        TextPersist(output, "act-000001", "evt-9"), persisted, /*budget=*/4096, harness.session_dir);
    REQUIRE(request.channels.size() == 1);
    const std::string& display_path = request.channels.front().display_path;
    CHECK_FALSE(display_path.empty());
    CHECK(display_path.rfind("artifacts/", 0) != 0);  // 不是裸相对路径
    // display_path 解析后必须落在本场 session 目录内(作用域:文件真身
    // 所在),且与 result_ref 的相对路径指向同一文件。
    const std::filesystem::path resolved = lubancode::platform::Utf8ToPath(display_path);
    const std::string relative = persisted.result_ref.at(1).value("path", std::string());
    std::error_code ec;
    CHECK(std::filesystem::equivalent(resolved, harness.session_dir /
                                                     lubancode::platform::Utf8ToPath(relative),
                                      ec));

    const auto preview = BuildToolPreview(request);
    REQUIRE(preview.truncated);
    // 中段证据不在预览里(这正是追回口存在的理由)。
    CHECK(preview.text.find("needle_retention_marker") == std::string::npos);
    CHECK(preview.full_output.size() == 1);
    // 预览指引:绝对路径 + read_file 分段读 + sha256 在账。
    CHECK(preview.text.find("retrieval_hint") != std::string::npos);
    CHECK(preview.text.find("read_file") != std::string::npos);
    CHECK(preview.text.find(display_path) != std::string::npos);
    // 按预览给的路径读回原文:中段证据在场,sha256 与账上 result_ref 对上
    // (read_file 端不重复校验,hash 真值在 tool.result.persisted 的六键里)。
    const std::string read_back = ReadFileBytes(resolved);
    CHECK(read_back == output);
    CHECK(read_back.find("needle_retention_marker") != std::string::npos);
    CHECK(lubancode::hooks::Sha256Hex(read_back) ==
          persisted.result_ref.at(1).value("sha256", std::string()));
}

TEST_CASE("追回链:未超预算的预览就是全文,不给 retrieval_hint") {
    RetrievalHarness harness("inline");
    const std::string output = "short output, fits the budget";
    auto store_or = harness.Open();
    REQUIRE(store_or.has_value());
    const auto persisted = store_or->Persist(TextPersist(output, "act-000002", "evt-8"));
    REQUIRE(persisted.ok);

    const auto request =
        PreviewFromPersistedMaterials(TextPersist(output, "act-000002", "evt-8"), persisted,
                                       /*budget=*/32768, harness.session_dir);
    const auto preview = BuildToolPreview(request);
    CHECK_FALSE(preview.truncated);
    CHECK(preview.text.find(output) != std::string::npos);
    CHECK(preview.text.find("retrieval_hint") == std::string::npos);
}

TEST_CASE("追回链:文件缺失不冒充——display_path 指向的文件被删后如实缺席") {
    RetrievalHarness harness("missing");
    const std::string output = LongCombinedOutput();
    auto store_or = harness.Open();
    REQUIRE(store_or.has_value());
    const auto persisted = store_or->Persist(TextPersist(output, "act-000003", "evt-7"));
    REQUIRE(persisted.ok);
    const std::string relative = persisted.result_ref.at(1).value("path", std::string());
    const std::string sha_on_ledger = persisted.result_ref.at(1).value("sha256", std::string());

    // 会话目录整场删除(T15 的 /delete)后:按 display_path 读回时文件已
    // 不在——read_file 报"文件不存在",不冒充成功读取;账上的 sha256 仍
    // 是定位原文的对账锚(审计侧 ExpandResultPreview 按它探缺口)。
    // display_path 从不指向仓外,作用域随本场目录存灭。
    std::error_code ec;
    std::filesystem::remove_all(harness.session_dir, ec);
    const std::filesystem::path resolved =
        harness.session_dir / lubancode::platform::Utf8ToPath(relative);
    CHECK_FALSE(std::filesystem::exists(resolved, ec));
    CHECK(sha_on_ledger == lubancode::hooks::Sha256Hex(output));
}
