// wire fixture 库的读取口(模型协议兼容实录矩阵单,P0):
//
// tests/fixtures/api/<wire>/<fixture_id>.sse  脱敏 wire 实录(SSE 原始字节)
// tests/fixtures/api/<wire>/<fixture_id>.json manifest(来源/场景/期望)
//
// 规矩(工单明文):
//   - 正文可换短合成字样;事件类型、字段、null/空串、索引、次序、usage
//     形状不得改;
//   - 钥匙、request id、业务正文一律脱敏;
//   - 读不动、缺 manifest、重复 id,测试当场红;
//   - source_document 是登记在册的手册(三份根下 wire 兼容手册 + docs 下
//     端点兼容手册)之一时,doc_snapshot_hash 必填且须与当前手册文件的
//     sha256 一致——手册一变,对账测试红,提醒人核对 fixture 后更新 hash。
//     internal 来源(仓库缩样/合成)不记手册 hash。

#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode_test {

// 一册 wire fixture:manifest 字段 + 实录字节。
struct ApiFixture {
    std::string fixture_id;
    std::string wire;  // anthropic-messages / openai-chat-completions / openai-responses / google-generate-content
    std::string provider;
    std::string model;
    std::string scenario;
    std::string source_document;  // 手册文件名或 "internal"
    std::string source_section;   // 手册段落名(手册来源必填)
    std::string captured_at;      // YYYY-MM-DD
    std::string doc_snapshot_hash;  // 手册 sha256(hex);internal 来源为空
    nlohmann::json request_expectation = nlohmann::json::object();
    std::vector<std::string> expected_events;
    std::vector<std::string> expected_replay;
    nlohmann::json usage_expectation = nlohmann::json::object();
    std::string stop_reason;
    std::string notes;
    std::string stream;  // .sse 的原始字节(含 data: 行与空行分隔)

    // 实录切成 SSE 帧(走 api::SseFramer,与生产同一切法;半帧/并帧由
    // 调用方自己拆 stream 再喂 framer,L3 的四种切法在测试侧做)。
    // 返回 (event 名, data JSON 文本) 对;没有 event 名的帧 event 为空。
    std::vector<std::pair<std::string, std::string>> SseFrames() const;
};

// 读一只 fixture。wire_dir 是四个 wire 子目录名,id 不带扩展名。
std::expected<ApiFixture, std::string> LoadApiFixture(const std::string& wire_dir,
                                                      const std::string& id);

// 扫全 tests/fixtures/api/:逐册加载 + 必填校验 + 全库 id 查重。
// 任何一册坏了整批报错(错误信息带 fixture 坐标)。
std::expected<std::vector<ApiFixture>, std::string> LoadAllApiFixtures();

// 文件 sha256(小写 hex)。文件读不动返回空串。
std::string Sha256File(const std::filesystem::path& path);

// 手册的绝对路径(LUBANCODE_SOURCE_DIR 根下,可带相对子路径如
// docs/features/providers/vllm.md)。文件名带中文,Windows 下必须走宽路径
// (fs::path(narrow) 会过 ACP,打不开)。
std::filesystem::path ManualPath(const std::string& filename);

// 手册 fixture 来源登记:手册文件名 -> 首批来源段清单(fixture 对账测试
// 用它核 hash)。新增手册来源时在这里登记,没登记的手册名 parse 即拒。
const std::vector<std::string>& ManualSourceDocuments();

}  // namespace lubancode_test
