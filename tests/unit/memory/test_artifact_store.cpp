// 渐进式上下文仓(第二期:可追回 artifact)的单测:分块边界(UTF-8 不劈
// 码点/Markdown 按标题/JSON 结构行/日志行窗)、原子落盘与幂等、hash 校验
// 与隔离、断点重开、Read 预算拒绝、CompressWorkingView 落仓接线、两把
// 只读钥匙的 scope(规格"测试"节)。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "agent/artifact_store.hpp"
#include "agent/context_events.hpp"
#include "tools/context_tools.hpp"

namespace {

using lubancode::agent::ArtifactChunk;
using lubancode::agent::ArtifactContentKind;
using lubancode::agent::ContextArtifactStore;
using lubancode::agent::DetectArtifactKind;

std::string MakeLongLog(std::size_t lines) {
    std::string out;
    for (std::size_t i = 1; i <= lines; ++i) {
        out += "[12:00:" + std::to_string(i % 60 < 10 ? 0 : i % 60) + "] build step " + std::to_string(i) +
               " ok\n";
    }
    return out;
}

class TempStoreDir {
public:
    TempStoreDir(const std::string& name)
        : path_(std::filesystem::temp_directory_path() / name) {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_, ec);
    }
    ~TempStoreDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace

TEST_CASE("内容类型识别") {
    CHECK(DetectArtifactKind("run_command", "whatever") == ArtifactContentKind::CommandLog);
    CHECK(DetectArtifactKind("read_file", "{\"a\":1}") == ArtifactContentKind::Json);
    CHECK(DetectArtifactKind("read_file", "# 标题\n正文") == ArtifactContentKind::Markdown);
    CHECK(DetectArtifactKind("read_file", "普通几行\n文本") == ArtifactContentKind::Text);
}

TEST_CASE("分块:Markdown 按标题,块号稳定、行号从 1 起") {
    std::string content;
    for (int section = 1; section <= 3; ++section) {
        content += "# 第" + std::to_string(section) + "节\n";
        for (int i = 0; i < 5; ++i) {
            content += "内容行 " + std::to_string(section) + "-" + std::to_string(i) + "\n";
        }
    }
    const auto chunks = lubancode::agent::ChunkArtifact(content, ArtifactContentKind::Markdown);
    REQUIRE(chunks.size() == 3);
    CHECK(chunks[0].chunk_id == "c0");
    CHECK(chunks[0].line_start == 1);
    CHECK(chunks[0].line_count == 6);
    CHECK(chunks[1].line_start == 7);
    CHECK(chunks[2].line_start == 13);
    CHECK(chunks[0].heading.find("第1节") != std::string::npos);
    // 相邻块字节范围无缝衔接(全文可按块拼回)。
    CHECK(chunks[0].byte_end == chunks[1].byte_start);
    CHECK(chunks[2].byte_end == content.size());
}

TEST_CASE("分块:日志按行窗,UTF-8 绝不从码点中腰劈开") {
    // 每行带中文(3 字节码点),行窗切满后边界必在行首。
    std::string content;
    for (int i = 0; i < 200; ++i) {
        content += "构建日志行" + std::to_string(i) + ":一切正常,中文内容不劈开\n";
    }
    const auto chunks = lubancode::agent::ChunkArtifact(content, ArtifactContentKind::CommandLog, 512);
    REQUIRE(chunks.size() > 1);
    for (const auto& chunk : chunks) {
        // 边界字节必须是码点起点(不是 10xxxxxx 续字节)。
        CHECK((static_cast<unsigned char>(content[chunk.byte_start]) & 0xC0) != 0x80);
        CHECK(chunk.byte_end <= content.size());
        if (&chunk != &chunks.back()) {
            CHECK(chunk.byte_end < content.size());
            CHECK((static_cast<unsigned char>(content[chunk.byte_end]) & 0xC0) != 0x80);
        }
    }
    // 按块拼回 == 原文(真本守恒)。
    std::string reassembled;
    for (const auto& chunk : chunks) {
        reassembled += content.substr(chunk.byte_start, chunk.byte_end - chunk.byte_start);
    }
    CHECK(reassembled == content);
}

TEST_CASE("分块:JSON 按顶层结构行,数组元素窗口不劈括号") {
    const std::string content = R"({
  "items": [
    {"id": 1, "name": "alpha"},
    {"id": 2, "name": "beta"},
    {"id": 3, "name": "gamma"}
  ],
  "total": 3
}
)";
    const auto chunks = lubancode::agent::ChunkArtifact(content, ArtifactContentKind::Json, 40);
    REQUIRE(!chunks.empty());
    // 行号/字节范围合法(值域)。
    std::size_t covered = 0;
    for (const auto& chunk : chunks) {
        covered += chunk.byte_end - chunk.byte_start;
        CHECK(chunk.line_start >= 1);
        CHECK(chunk.line_count >= 1);
    }
    CHECK(covered == content.size());
}

TEST_CASE("检索:命中行、排序、中文原文、预览不劈码点") {
    const std::string content = "一切正常\n这里有个 error: boom\n又一正常行\nerror again: error twice\n";
    const auto found = lubancode::agent::SearchArtifactContent(content, {}, "error", 8);
    REQUIRE(found.size() == 2);
    CHECK(found[0].line == 4);  // 命中两次的行排前
    CHECK(found[0].score == 2);
    CHECK(found[1].line == 2);
    CHECK(found[1].snippet.find("boom") != std::string::npos);
    // 中文查询按原文子串。
    const auto zh = lubancode::agent::SearchArtifactContent("第一行正常\n第二行异常\n", {}, "异常", 8);
    REQUIRE(zh.size() == 1);
    CHECK(zh[0].line == 2);
    // max_results 封顶。
    const auto many = lubancode::agent::SearchArtifactContent(MakeLongLog(50), {}, "build", 5);
    CHECK(many.size() == 5);
}

TEST_CASE("仓:原子落盘、幂等、断点重开、Close") {
    TempStoreDir dir("lubancode-artifact-store-basic");
    ContextArtifactStore store;
    CHECK(!store.active());
    CHECK(store.Open((dir.path() / "ctx").string(), "sess-1"));
    CHECK(store.active());

    const std::string content = MakeLongLog(400);  // 约 10KB
    const auto ref = store.Offload("toolu-1", "run_command", content, 3);
    REQUIRE(ref.has_value());
    CHECK(ref->artifact_id == "a0001");
    CHECK(ref->bytes == content.size());
    CHECK(ref->lines == 400);
    CHECK(ref->session_id == "sess-1");
    // 磁盘布局:blob + chunks + index 各就各位。
    CHECK(std::filesystem::exists(dir.path() / "ctx" / ref->blob_path));
    CHECK(std::filesystem::exists(dir.path() / "ctx" / ref->chunk_index_path));
    CHECK(std::filesystem::exists(dir.path() / "ctx" / "index.jsonl"));
    // 幂等:同 tool_use_id 再卸,还旧 ref 不加新行。
    const auto again = store.Offload("toolu-1", "run_command", content, 3);
    REQUIRE(again.has_value());
    CHECK(again->artifact_id == ref->artifact_id);
    CHECK(store.refs().size() == 1);
    // 第二枚接着编号。
    const auto second = store.Offload("toolu-2", "read_file", "short but forced", 7);
    REQUIRE(second.has_value());
    CHECK(second->artifact_id == "a0002");

    // 断点重开:编号与索引接着走(resume 同会话续卸)。
    ContextArtifactStore reopened;
    CHECK(reopened.Open((dir.path() / "ctx").string(), "sess-1"));
    CHECK(reopened.refs().size() == 2);
    CHECK(reopened.Find("a0001") != nullptr);
    const auto third = reopened.Offload("toolu-3", "read_file", "第三枚", 9);
    REQUIRE(third.has_value());
    CHECK(third->artifact_id == "a0003");

    // Close:/clear 后旧 id 查不到。
    reopened.Close();
    CHECK(!reopened.active());
    CHECK(reopened.Find("a1") == nullptr);
}

TEST_CASE("仓:hash 不合立即隔离,读不到不供给") {
    TempStoreDir dir("lubancode-artifact-store-hash");
    ContextArtifactStore store;
    REQUIRE(store.Open((dir.path() / "ctx").string(), "sess-1"));
    const auto ref = store.Offload("toolu-1", "run_command", MakeLongLog(300), 1);
    REQUIRE(ref.has_value());

    // 篡改 blob:hash 校验拦下。
    {
        std::ofstream tamper((dir.path() / "ctx" / ref->blob_path).string(), std::ios::binary | std::ios::trunc);
        tamper << "被改过的内容";
    }
    std::string error;
    CHECK(!store.ReadBlobVerified(*ref, &error).has_value());
    CHECK(error.find("hash") != std::string::npos);
    CHECK(!store.Read(*ref, "", 1, 10).ok);
    CHECK(!store.Search(*ref, "build", 5, &error).has_value());

    // hash 门先于供给:检索/读取都不给内容(隔离)。
}

TEST_CASE("Read:按块、按行窗、预算拒绝给可用范围") {
    TempStoreDir dir("lubancode-artifact-store-read");
    ContextArtifactStore store;
    REQUIRE(store.Open((dir.path() / "ctx").string(), "sess-1"));
    const std::string content = MakeLongLog(600);
    const auto ref = store.Offload("toolu-1", "run_command", content, 1);
    REQUIRE(ref.has_value());

    SUBCASE("行窗") {
        const auto result = store.Read(*ref, "", 10, 5);
        REQUIRE(result.ok);
        CHECK(result.line_start == 10);
        CHECK(result.line_count == 5);
        CHECK(result.text.find("build step 10") != std::string::npos);
        CHECK(result.text.find("build step 15") == std::string::npos);  // 不多给
    }
    SUBCASE("line_count=0 读到结尾") {
        const auto result = store.Read(*ref, "", 598, 0);
        REQUIRE(result.ok);
        CHECK(result.line_count == 3);
    }
    SUBCASE("越界拒绝并给可用范围") {
        const auto result = store.Read(*ref, "", 99999, 5);
        CHECK(!result.ok);
        CHECK(!result.available.empty());
        CHECK(result.available.find("600") != std::string::npos);
    }
    SUBCASE("按块读") {
        const auto chunks = store.ChunksFor(*ref);
        REQUIRE(!chunks.empty());
        const auto result = store.Read(*ref, chunks[0].chunk_id, 0, 0);
        REQUIRE(result.ok);
        CHECK(result.chunk_id == chunks[0].chunk_id);
        CHECK(result.line_start == chunks[0].line_start);
        CHECK(result.line_count == chunks[0].line_count);
        const auto missing = store.Read(*ref, "cZ9", 0, 0);
        CHECK(!missing.ok);
    }
    SUBCASE("超预算拒绝,不悄悄截") {
        const auto result = store.Read(*ref, "", 1, 600, /*max_bytes=*/200);
        CHECK(!result.ok);
        CHECK(result.error.find("预算") != std::string::npos);
        CHECK(!result.available.empty());
    }
}

TEST_CASE("CompressWorkingView 带仓:长结果落盘、视图带稳定 id;落盘失败保全文") {
    // 造一条带超长 tool result 的历史(read_file 有判重键)。
    std::vector<lubancode::api::Message> history;
    lubancode::api::Message user;
    user.role = lubancode::api::Role::User;
    user.content.push_back(lubancode::api::TextBlock{"看看这份日志"});
    history.push_back(user);
    lubancode::api::Message assistant;
    assistant.role = lubancode::api::Role::Assistant;
    lubancode::api::ToolUseBlock use;
    use.id = "toolu-long";
    use.name = "read_file";
    use.input = nlohmann::json{{"path", "build.log"}};
    assistant.content.push_back(use);
    history.push_back(assistant);
    lubancode::api::Message results;
    results.role = lubancode::api::Role::User;
    const std::string long_content = MakeLongLog(900);
    results.content.push_back(lubancode::api::ToolResultBlock{"toolu-long", long_content, false});
    history.push_back(results);

    lubancode::agent::StructuralCompressionOptions options;
    options.long_result_bytes = 4096;

    SUBCASE("有仓:blob 落盘,视图带 artifact_id 与检索指引") {
        TempStoreDir dir("lubancode-artifact-view");
        ContextArtifactStore store;
        REQUIRE(store.Open((dir.path() / "ctx").string(), "sess-1"));
        lubancode::agent::StructuralCompressionStats stats;
        lubancode::agent::ResultViewMemo memo;
        const auto view =
            lubancode::agent::CompressWorkingView(history, options, stats, memo, &store);
        REQUIRE(stats.offloaded_results == 1);
        // 视图里能找到稳定 id 与两把钥匙的指引。
        std::string view_text;
        for (const auto& block : view[2].content) {
            if (const auto* result = std::get_if<lubancode::api::ToolResultBlock>(&block)) {
                view_text = result->content;
            }
        }
        CHECK(view_text.find("a0001") != std::string::npos);
        CHECK(view_text.find("context_search") != std::string::npos);
        CHECK(view_text.find("context_read") != std::string::npos);
        CHECK(view_text.size() < long_content.size() / 2);
        // 仓里追得回全文(原文不丢)。
        const auto* ref = store.Find("a0001");
        REQUIRE(ref != nullptr);
        const auto blob = store.ReadBlobVerified(*ref);
        REQUIRE(blob.has_value());
        CHECK(*blob == long_content);
        // memo 记住了决策:重放不再重复落盘。
        lubancode::agent::StructuralCompressionStats stats2;
        const auto view2 = lubancode::agent::CompressWorkingView(history, options, stats2, memo, &store);
        CHECK(stats2.offloaded_results == 0);  // 旧决策重放,不算新做
        CHECK(view2.size() == view.size());
        CHECK(store.refs().size() == 1);
    }

    SUBCASE("仓没开:落盘失败保内存全文(规格\"原文不丢\"),不产生假引用") {
        ContextArtifactStore closed_store;  // 未 Open = 落盘必失败
        lubancode::agent::StructuralCompressionStats stats;
        lubancode::agent::ResultViewMemo memo;
        const auto view =
            lubancode::agent::CompressWorkingView(history, options, stats, memo, &closed_store);
        // 决策退回 Full:不换预览引用(那是指着不存在的真本),全文照旧发。
        CHECK(stats.offloaded_results == 0);
        const auto* view_result = std::get_if<lubancode::api::ToolResultBlock>(&view[2].content[0]);
        REQUIRE(view_result != nullptr);
        CHECK(view_result->content == long_content);
        // 活历史一字未动(真本还在)。
        CHECK(std::get_if<lubancode::api::ToolResultBlock>(&history[2].content[0])->content == long_content);
        CHECK(closed_store.refs().empty());  // 没有登记行,没编号,不追回
    }

    SUBCASE("仓指针为空:完全不启用第二期,视图仍是预览形态(旧行为)") {
        lubancode::agent::StructuralCompressionStats stats;
        lubancode::agent::ResultViewMemo memo;
        const auto view = lubancode::agent::CompressWorkingView(history, options, stats, memo);
        CHECK(stats.offloaded_results == 1);
        const auto* view_result = std::get_if<lubancode::api::ToolResultBlock>(&view[2].content[0]);
        REQUIRE(view_result != nullptr);
        CHECK(view_result->content.find("全文在会话存档") != std::string::npos);
    }
}

TEST_CASE("两把只读钥匙:scope 只认稳定 id") {
    TempStoreDir dir("lubancode-artifact-tools");
    auto store = std::make_shared<ContextArtifactStore>();
    lubancode::tools::ContextSearchTool search(store);
    lubancode::tools::ContextReadTool read(store);

    SUBCASE("仓没开:友好错误,不是崩") {
        const auto result = search.execute(nlohmann::json{{"artifact_id", "a1"}, {"query", "x"}});
        CHECK(result.is_error);
        const auto read_result = read.execute(nlohmann::json{{"artifact_id", "a1"}, {"line_start", 1}});
        CHECK(read_result.is_error);
    }

    REQUIRE(store->Open((dir.path() / "ctx").string(), "sess-1"));
    const auto ref = store->Offload("toolu-1", "run_command", MakeLongLog(300), 1);
    REQUIRE(ref.has_value());

    SUBCASE("别的会话/不存在的 id 查不到") {
        const auto result = search.execute(nlohmann::json{{"artifact_id", "a999"}, {"query", "build"}});
        CHECK(result.is_error);
        CHECK(result.content.find("找不到") != std::string::npos);
    }
    SUBCASE("搜到再读:闭环") {
        const auto result =
            search.execute(nlohmann::json{{"artifact_id", ref->artifact_id}, {"query", "build step 42"}});
        REQUIRE(!result.is_error);
        CHECK(result.content.find("行 ") != std::string::npos);
        const auto read_result = read.execute(
            nlohmann::json{{"artifact_id", ref->artifact_id}, {"line_start", 1}, {"line_count", 3}});
        REQUIRE(!read_result.is_error);
        CHECK(read_result.content.find("第 1-3 行") != std::string::npos);
    }
    SUBCASE("read 不吃磁盘路径:参数里只有 id/块/行") {
        // schema 上根本没有 path 字段;给多余字段也不当路径用。
        const auto result = read.execute(nlohmann::json{{"artifact_id", ref->artifact_id},
                                                        {"line_start", 1},
                                                        {"line_count", 1},
                                                        {"path", "C:/Windows/system.ini"}});
        REQUIRE(!result.is_error);
        CHECK(result.content.find("system.ini") == std::string::npos);
    }
}
