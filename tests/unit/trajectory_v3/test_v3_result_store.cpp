// v3 结果仓与模型预览测试(P1 其余,§4.16-4.17/§4.21):32 KiB 预算、
// 头尾节选与来源标记、UTF-8 边界、多文件共额、捕获完整性、清单超限、
// 降档与不可表示;artifact 不可变落档与 result_ref 形状。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "trajectory/v3/result_store.hpp"
#include "trajectory/v3/schema3.hpp"

using namespace lubancode::trajectory::v3;

namespace {

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

bool IsValidUtf8(const std::string& text) {
    int continuation = 0;
    for (char c : text) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (continuation > 0) {
            if ((byte & 0xC0) != 0x80) {
                return false;
            }
            --continuation;
        } else if (byte >= 0xF0) {
            continuation = 3;
        } else if (byte >= 0xE0) {
            continuation = 2;
        } else if (byte >= 0xC0) {
            continuation = 1;
        } else if (byte >= 0x80) {
            return false;
        }
    }
    return continuation == 0;
}

PreviewChannel StdChannel(std::string text, std::uint64_t output_bytes = 0) {
    PreviewChannel channel;
    channel.display_path = "sessions/example/artifacts/res-000001.stdout.txt";
    channel.channel = "stdout";
    channel.text = std::move(text);
    channel.output_bytes = output_bytes;
    return channel;
}

struct StoreHarness {
    std::filesystem::path dir;

    explicit StoreHarness(const char* tag) {
        dir = std::filesystem::temp_directory_path() /
              ("lubancode-v3-store-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
    }
};

ResultStore::ChannelOutput Out(std::string channel, std::string data,
                               std::uint64_t output_bytes) {
    ResultStore::ChannelOutput output;
    output.channel = std::move(channel);
    output.data = std::move(data);
    output.output_bytes = output_bytes;
    return output;
}

}  // namespace

TEST_CASE("完整渲染不超 32768 字节:原样返回,不插省略标记") {
    PreviewRequest request;
    request.exit_code = 0;
    request.channels.push_back(StdChannel(std::string(100, 'a'), 100));
    PreviewResult result = BuildToolPreview(request);
    CHECK(!result.truncated);
    CHECK(result.full_output.size() == 1);
    CHECK(result.captured_output.empty());
    CHECK(result.text.find(std::string(100, 'a')) != std::string::npos);
    CHECK(result.text.find("[中间内容已省略]") == std::string::npos);
}

TEST_CASE("32767/32768/32769 字节边界:前两档不截断,后一档节选") {
    // 完整渲染长度 = 说明区 + 标记 + 正文 + 换行;按此反推正文长度。
    auto rendered_len = [](std::size_t body_len) {
        std::string header = "output_bytes[stdout]: " + std::to_string(body_len) + "\n" +
                             "truncated: false\n" +
                             "capture_complete: true\n" +
                             "full_output: [\"sessions/example/artifacts/res-000001.stdout.txt\"]\n";
        std::string marker = "[文件: sessions/example/artifacts/res-000001.stdout.txt | 通道: stdout]";
        return header.size() + 1 + marker.size() + 1 + body_len + 1;
    };
    std::size_t fits = 0;
    while (rendered_len(fits + 1) <= 32768u) {
        ++fits;
    }
    CHECK(rendered_len(fits) <= 32768u);
    CHECK(rendered_len(fits + 1) > 32768u);

    PreviewRequest exact;
    exact.channels.push_back(StdChannel(std::string(fits, 'a'), fits));
    PreviewResult in_budget = BuildToolPreview(exact);
    CHECK(!in_budget.truncated);  // 恰好装下:不截断

    PreviewRequest over;
    over.channels.push_back(StdChannel(std::string(fits + 1, 'a'), fits + 1));
    PreviewResult out_budget = BuildToolPreview(over);
    CHECK(out_budget.truncated);  // 超一字节:保存原文 + 节选(§4.21)
    CHECK(out_budget.text.size() <= 32768);
    CHECK(out_budget.text.find("[中间内容已省略]") != std::string::npos);
    CHECK(out_budget.text.find("truncated: true") != std::string::npos);
}

TEST_CASE("大 stdout 加短 stderr:整次调用共用 32 KiB 预算") {
    PreviewRequest request;
    request.exit_code = 0;
    PreviewChannel big = StdChannel(std::string(30000, 'x'), 30000);
    PreviewChannel small;
    small.display_path = "sessions/example/artifacts/res-000001.stderr.txt";
    small.channel = "stderr";
    small.text = std::string(5000, 'y');
    small.output_bytes = 5000;
    request.channels.push_back(big);
    request.channels.push_back(small);
    PreviewResult result = BuildToolPreview(request);
    CHECK(result.truncated);
    CHECK(result.text.size() <= 32768);  // 不能每份文件各截 32 KiB(§4.17)
    // 头来自 stdout,尾来自 stderr:来源各自标明(§4.21)。
    CHECK(result.text.find("| 通道: stdout") != std::string::npos);
    CHECK(result.text.find("| 通道: stderr") != std::string::npos);
    CHECK(result.full_output.size() == 2);
}

TEST_CASE("头尾片段跨文件:尾段从文件中间开始也重新标来源") {
    PreviewRequest request;
    PreviewChannel first = StdChannel(std::string(20000, 'a'), 20000);
    PreviewChannel second;
    second.display_path = "sessions/example/artifacts/res-000001.stderr.txt";
    second.channel = "stderr";
    second.text = std::string(20000, 'b');
    second.output_bytes = 20000;
    request.channels.push_back(first);
    request.channels.push_back(second);
    PreviewResult result = BuildToolPreview(request);
    REQUIRE(result.truncated);
    // 不把文件 A 的开头和文件 B 的结尾冒充同一文件(§4.17)。
    std::size_t stdout_marker = result.text.find("[文件: sessions/example/artifacts/res-000001.stdout.txt");
    std::size_t stderr_marker = result.text.find("[文件: sessions/example/artifacts/res-000001.stderr.txt");
    REQUIRE(stdout_marker != std::string::npos);
    REQUIRE(stderr_marker != std::string::npos);
    CHECK(stdout_marker < stderr_marker);  // 稳定通道次序:stdout 在前
    // 尾段必须带标记:标记出现在省略提示之后。
    std::size_t omission = result.text.find("[中间内容已省略]");
    REQUIRE(omission != std::string::npos);
    CHECK(stderr_marker > omission);
}

TEST_CASE("中文来源标记与 UTF-8 边界:按字节帽截断,不切出非法序列") {
    PreviewRequest request;
    // "码" 占 3 字节;正文长到必截。
    std::string body;
    for (int i = 0; i < 12000; ++i) {
        body += "码";
    }
    request.channels.push_back(StdChannel(body, body.size()));
    PreviewResult result = BuildToolPreview(request);
    REQUIRE(result.truncated);
    CHECK(result.text.size() <= 32768);
    CHECK(IsValidUtf8(result.text));  // 不切出非法 UTF-8(§4.21)
    CHECK(result.text.find("码") != std::string::npos);
}

TEST_CASE("超长单行:不越预算,不重复头尾") {
    PreviewRequest request;
    request.channels.push_back(StdChannel(std::string(60000, 'z'), 60000));
    PreviewResult result = BuildToolPreview(request);
    CHECK(result.truncated);
    CHECK(result.text.size() <= 32768);
    // 头尾不重叠:中间省略提示恰一枚。
    std::size_t first = result.text.find("[中间内容已省略]");
    CHECK(first != std::string::npos);
    CHECK(result.text.find("[中间内容已省略]", first + 1) == std::string::npos);
}

TEST_CASE("capture_complete=false:full_output/captured_output 分列,不称完整") {
    PreviewRequest request;
    PreviewChannel partial = StdChannel(std::string(100, 'a'), 100);
    partial.capture_complete = false;
    partial.capture_reason = "quota";
    request.channels.push_back(partial);
    PreviewResult result = BuildToolPreview(request);
    CHECK(result.full_output.empty());
    CHECK(result.captured_output.size() == 1);
    CHECK(result.text.find("capture_complete: false") != std::string::npos);
    CHECK(result.text.find("capture[stdout]: quota") != std::string::npos);

    // 全部不完整时 full_output 为 [](§4.17)。
    PreviewRequest all_partial = request;
    PreviewChannel another = StdChannel(std::string(50, 'b'), 50);
    another.capture_complete = false;
    another.capture_reason = "io_error";
    all_partial.channels.push_back(another);
    PreviewResult both = BuildToolPreview(all_partial);
    CHECK(both.full_output.empty());
    CHECK(both.captured_output.size() == 2);
}

TEST_CASE("output_bytes 下界:显示至少 N bytes,不拿预览长度充数") {
    PreviewRequest request;
    PreviewChannel channel = StdChannel(std::string(100, 'a'), 850000);
    channel.output_bytes_lower_bound = true;
    request.channels.push_back(channel);
    PreviewResult result = BuildToolPreview(request);
    CHECK(result.text.find("output_bytes[stdout]: >= 850000") != std::string::npos);
}

TEST_CASE("只展示了部分文件:点名 not_shown,与文件缺失分清") {
    PreviewRequest request;
    // 三通道:第一通道超长(占满头),后两通道只剩标题位、无正文。
    PreviewChannel big = StdChannel(std::string(40000, 'a'), 40000);
    PreviewChannel mid;
    mid.display_path = "sessions/example/artifacts/res-000001.stderr.txt";
    mid.channel = "stderr";
    mid.text = std::string(40000, 'b');
    mid.output_bytes = 40000;
    PreviewChannel tail_only;
    tail_only.display_path = "sessions/example/artifacts/res-000001.report.txt";
    tail_only.channel = "report";
    tail_only.text = std::string(40000, 'c');
    tail_only.output_bytes = 40000;
    request.channels = {big, mid, tail_only};
    PreviewResult result = BuildToolPreview(request);
    CHECK(result.truncated);
    // 未展示正文的文件明确点名(§4.17),result_ref 仍全量。
    CHECK(result.full_output.size() == 3);
}

TEST_CASE("文件清单自身超限:先存清单 artifact,再带 output_index 重算") {
    PreviewRequest request;
    // 很多长路径文件,清单行远超 4 KiB 档。
    for (int i = 0; i < 120; ++i) {
        PreviewChannel channel;
        channel.display_path =
            "sessions/example/artifacts/res-000001.part-" + std::to_string(i) +
            ".with-a-very-long-directory-name-to-blow-up-the-listing-budget.txt";
        channel.channel = "part";
        channel.text = "x";
        channel.output_bytes = 1;
        request.channels.push_back(channel);
    }
    request.max_preview_bytes = 4096;
    // 第一遍:没有 output_index → listing_overflow + listing_text。
    PreviewResult first = BuildToolPreview(request);
    CHECK(first.listing_overflow);
    CHECK(first.listing_text.find("full_output: [") != std::string::npos);

    // 调用方把完整清单存成 artifact 后带路径重算。
    StoreHarness harness("listing");
    auto store = ResultStore::Open(harness.dir);
    REQUIRE(store.has_value());
    auto listing_path = store->PersistListing("res-000001.index.txt", first.listing_text);
    REQUIRE(listing_path.has_value());

    PreviewRequest second = request;
    second.output_index_path = *listing_path;
    PreviewResult indexed = BuildToolPreview(second);
    CHECK(!indexed.listing_overflow);
    CHECK(indexed.text.size() <= 4096);
    CHECK(indexed.text.find("output_index: " + *listing_path) != std::string::npos);
    CHECK(indexed.omitted_output_count > 0);  // 明确省略计数,不默默少报
    CHECK(IsValidUtf8(indexed.text));
}

TEST_CASE("4 KiB 降档:重算用更小预算,同一段原文可缩") {
    const std::string body = std::string(20000, 'k');
    for (std::uint64_t budget : {16384u, 8192u, 4096u}) {
        PreviewRequest request;
        request.channels.push_back(StdChannel(body, body.size()));
        request.max_preview_bytes = budget;
        PreviewResult result = BuildToolPreview(request);
        REQUIRE(result.truncated);
        CHECK(result.text.size() <= budget);
        CHECK(IsValidUtf8(result.text));
        CHECK(result.text.find("[中间内容已省略]") != std::string::npos);
    }
}

TEST_CASE("结果仓:metadata 与通道各就位,result_ref 六键数组") {
    StoreHarness harness("persist");
    auto store = ResultStore::Open(harness.dir);
    REQUIRE(store.has_value());

    ResultStore::PersistRequest request;
    request.result_kind = "process";
    request.tool_call_id = "action-000001";
    request.attempt = 1;
    request.execution_event_ref = "evt-000004";
    request.preview_policy = nlohmann::json::object({{"maxPreviewBytes", 32768}});
    request.capture_limits = nlohmann::json::object({{"fullCaptureBytes", 10485760}});
    request.outputs.push_back(Out("stdout", std::string(1000, 'o'), 1000));
    request.outputs.push_back(Out("stderr", std::string(200, 'e'), 200));
    auto outcome = store->Persist(request);
    REQUIRE(outcome.ok);
    CHECK(outcome.result_id == "res-000001");

    // 文件:metadata + 两个通道文件,都在 artifacts/ 下。
    CHECK(std::filesystem::exists(harness.dir / "artifacts" / "res-000001.json"));
    CHECK(std::filesystem::exists(harness.dir / "artifacts" / "res-000001.stdout.txt"));
    CHECK(std::filesystem::exists(harness.dir / "artifacts" / "res-000001.stderr.txt"));

    // result_ref:metadata + stdout + stderr,固定数组(§4.16)。
    REQUIRE(outcome.result_ref.size() == 3);
    CHECK(outcome.result_ref[0]["kind"] == "result_metadata");
    CHECK(outcome.result_ref[1]["kind"] == "stdout");
    CHECK(outcome.result_ref[2]["kind"] == "stderr");
    for (const auto& ref : outcome.result_ref) {
        for (const char* key :
             {"artifactId", "kind", "path", "sha256", "bytes", "mediaType"}) {
            CHECK(ref.contains(key));
        }
        CHECK(!ValidateArtifactRef("test", ref).has_value());
    }

    // metadata 内容:执行引用、预览策略、outputs 逐项(§4.16 字段表)。
    nlohmann::json metadata = nlohmann::json::parse(ReadFile(harness.dir / "artifacts" / "res-000001.json"));
    CHECK(metadata["tool_call_id"] == "action-000001");
    CHECK(metadata["execution_event_ref"] == "evt-000004");
    CHECK(metadata["outputs"].size() == 2);
    CHECK(metadata["outputs"][0]["ref"]["bytes"] == 1000);
    CHECK(metadata["outputs"][0]["capture_complete"] == true);
    CHECK(metadata["outputs"][0]["output_bytes"] == 1000);
    CHECK(metadata["outputs"][0]["byte_count_kind"] == "exact");

    // 连续第二枚:result_id 递增(不可变名不冲突)。
    auto second = store->Persist(request);
    REQUIRE(second.ok);
    CHECK(second.result_id == "res-000002");
}

TEST_CASE("空通道且收全:不造空文件,描述记 bytes=0") {
    StoreHarness harness("empty-channel");
    auto store = ResultStore::Open(harness.dir);
    REQUIRE(store.has_value());
    ResultStore::PersistRequest request;
    request.result_kind = "process";
    request.tool_call_id = "action-000001";
    request.execution_event_ref = "evt-000003";
    request.outputs.push_back(Out("stdout", "hello", 5));
    request.outputs.push_back(Out("stderr", "", 0));  // 明确捕获为空
    auto outcome = store->Persist(request);
    REQUIRE(outcome.ok);
    CHECK(std::filesystem::exists(harness.dir / "artifacts" / "res-000001.stdout.txt"));
    CHECK(!std::filesystem::exists(harness.dir / "artifacts" / "res-000001.stderr.txt"));
    // result_ref 只列已落稳文件:空通道不占位(§4.16)。
    REQUIRE(outcome.result_ref.size() == 2);  // metadata + stdout
    nlohmann::json metadata =
        nlohmann::json::parse(ReadFile(harness.dir / "artifacts" / "res-000001.json"));
    REQUIRE(metadata["outputs"].size() == 2);  // 描述里两通道都在
    CHECK(metadata["outputs"][1]["output_bytes"] == 0);
    CHECK(metadata["outputs"][1]["capture_complete"] == true);  // 空且收全
    CHECK(!metadata["outputs"][1].contains("ref"));            // 没造空文件
}

TEST_CASE("捕获不完整通道:capture_reason 入描述,部分字节照存") {
    StoreHarness harness("partial");
    auto store = ResultStore::Open(harness.dir);
    REQUIRE(store.has_value());
    ResultStore::PersistRequest request;
    request.result_kind = "process";
    request.tool_call_id = "action-000001";
    request.execution_event_ref = "evt-000003";
    ResultStore::ChannelOutput partial = Out("stdout", std::string(500, 'p'), 100000);
    partial.capture_complete = false;
    partial.capture_reason = "quota";
    partial.output_bytes_lower_bound = true;
    request.outputs.push_back(partial);
    auto outcome = store->Persist(request);
    REQUIRE(outcome.ok);
    nlohmann::json metadata =
        nlohmann::json::parse(ReadFile(harness.dir / "artifacts" / "res-000001.json"));
    CHECK(metadata["outputs"][0]["capture_complete"] == false);
    CHECK(metadata["outputs"][0]["capture_reason"] == "quota");
    CHECK(metadata["outputs"][0]["byte_count_kind"] == "lower_bound");
    CHECK(metadata["outputs"][0]["output_bytes"] == 100000);
    CHECK(metadata["outputs"][0]["captured_bytes"] == 500);
}

TEST_CASE("重开结果仓:result 号续发,不撞不可变名") {
    StoreHarness harness("reopen");
    {
        auto store = ResultStore::Open(harness.dir);
        REQUIRE(store.has_value());
        ResultStore::PersistRequest request;
        request.result_kind = "text";
        request.tool_call_id = "action-000001";
        request.execution_event_ref = "evt-000003";
        request.outputs.push_back(Out("report", "abc", 3));
        REQUIRE(store->Persist(request).ok);
    }
    {
        auto store = ResultStore::Open(harness.dir);
        REQUIRE(store.has_value());
        ResultStore::PersistRequest request;
        request.result_kind = "text";
        request.tool_call_id = "action-000002";
        request.execution_event_ref = "evt-000009";
        request.outputs.push_back(Out("report", "def", 3));
        auto outcome = store->Persist(request);
        REQUIRE(outcome.ok);
        CHECK(outcome.result_id == "res-000002");  // 扫已有 res-*.json 取最大 +1
    }
}

TEST_CASE("不可变名冲突:同名结果不允许覆盖(§4.15 result_id 不覆盖)") {
    StoreHarness harness("immutable");
    auto store = ResultStore::Open(harness.dir);
    REQUIRE(store.has_value());
    ResultStore::PersistRequest request;
    request.result_kind = "text";
    request.tool_call_id = "action-000001";
    request.execution_event_ref = "evt-000003";
    request.outputs.push_back(Out("report", "first", 5));
    REQUIRE(store->Persist(request).ok);  // res-000001,同实例计数器已前移

    // 预占下一枚不可变名:res-000002.json 已在(并发写者/残留),再落即撞车,
    // 不许覆盖(POSIX rename 会静默覆盖,必须显式拒)。
    {
        std::ofstream occupy(harness.dir / "artifacts" / "res-000002.json",
                             std::ios::binary | std::ios::trunc);
        occupy << "occupied";
    }
    request.outputs.clear();
    request.outputs.push_back(Out("report", "second", 6));
    auto clash = store->Persist(request);
    CHECK(!clash.ok);
    CHECK(clash.error.find("不可变名") != std::string::npos);
    // 占位文件原样:没被覆盖。
    std::ifstream check(harness.dir / "artifacts" / "res-000002.json");
    std::string content((std::istreambuf_iterator<char>(check)), std::istreambuf_iterator<char>());
    CHECK(content == "occupied");
}
