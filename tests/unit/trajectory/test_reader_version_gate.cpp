// 回滚纪律测试(P0-6 §十七回滚条款/§16.5):
//   - 旧 binary(不支持超前的 min_reader_version / envelope schema_version)
//     遇新账只准明拒读取:replay 报 replay.unsupported、verify 报
//     verify.unsupported_reader_version / schema.unsupported_version——
//     都不是 corrupt,也都不得改写 Journal 一枚字节。
//   - 用 v1 golden fixture 编辑后重建哈希链造"未来账":重链本身先拿
//     未编辑的原文对账(重链输出与 fixture 逐字节一致),保证拒读判的是
//     版本门,不是链没接对。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "trajectory/canonical_json.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/replay.hpp"

using namespace lubancode::trajectory;

#ifndef LUBANCODE_SOURCE_DIR
#define LUBANCODE_SOURCE_DIR "."
#endif

namespace {

std::filesystem::path GoldenFixture() {
    return std::filesystem::path(LUBANCODE_SOURCE_DIR) / "tests" / "fixtures" / "trajectory" /
           "v1" / "golden_main.jsonl";
}

std::vector<nlohmann::json> LoadGoldenLines() {
    std::ifstream file(GoldenFixture(), std::ios::binary);
    REQUIRE(file.is_open());
    std::vector<nlohmann::json> lines;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        lines.push_back(nlohmann::json::parse(line, nullptr, false));
        REQUIRE_FALSE(lines.back().is_discarded());
    }
    REQUIRE(lines.size() >= 2);
    return lines;
}

// 重建哈希链:逐行(prev_hash 接前一行,event_hash 按 canonical 去哈希
// 正文重算),输出即写盘的规范行。编辑只动 payload/schema_version,
// 链仍整——拒读若发生,判据只能是版本门。
std::vector<std::string> Rechain(const std::vector<nlohmann::json>& lines) {
    std::vector<std::string> out;
    out.reserve(lines.size());
    std::string prev_hash(kGenesisHash);
    for (const nlohmann::json& raw : lines) {
        nlohmann::json line = raw;
        line["prev_hash"] = prev_hash;
        line.erase("event_hash");
        const auto canonical = CanonicalJsonDump(line);
        REQUIRE(canonical.has_value());
        const std::string hash = ComputeEventHash(prev_hash, *canonical);
        line["event_hash"] = hash;
        const auto full = CanonicalJsonDump(line);
        REQUIRE(full.has_value());
        out.push_back(*full);
        prev_hash = hash;
    }
    return out;
}

std::filesystem::path WriteTempSession(const std::vector<std::string>& lines,
                                       const std::filesystem::path& parent, const char* name) {
    std::error_code ec;
    const std::filesystem::path session_dir = parent / name;
    REQUIRE(std::filesystem::create_directories(session_dir, ec));
    std::ofstream file(session_dir / "main.jsonl", std::ios::binary | std::ios::trunc);
    REQUIRE(file.is_open());
    for (const std::string& line : lines) {
        file << line << "\n";
    }
    return session_dir;
}

std::string ReadFileText(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

}  // namespace

TEST_CASE("回滚纪律: 重链对照——未编辑的 golden 重建后逐字节一致") {
    const std::vector<std::string> rebuilt = Rechain(LoadGoldenLines());
    const std::string original = ReadFileText(GoldenFixture());
    std::string joined;
    for (const std::string& line : rebuilt) {
        joined += line + "\n";
    }
    CHECK(joined == original);
}

TEST_CASE("回滚纪律: 超前 min_reader_version 明拒读且不改写 Journal") {
    std::vector<nlohmann::json> lines = LoadGoldenLines();
    REQUIRE(lines[0].value("kind", std::string()) == "run.started");
    lines[0]["payload"]["min_reader_version"] = 3;  // 读者最高认 2:未来账

    const auto parent = std::filesystem::temp_directory_path() / "lubancode-reader-gate-min";
    std::error_code ec;
    std::filesystem::remove_all(parent, ec);
    const std::filesystem::path session_dir = WriteTempSession(Rechain(lines), parent, "s1");
    const std::filesystem::path stream = session_dir / "main.jsonl";
    const std::string bytes_before = ReadFileText(stream);

    SUBCASE("replay: replay.unsupported,不判 corrupt") {
        const auto fold = FoldStreamReplay(stream);
        REQUIRE_FALSE(fold.ok());
        CHECK(fold.error_code == "replay.unsupported");
        CHECK(fold.message.find("min_reader_version=3") != std::string::npos);
    }
    SUBCASE("verify: verify.unsupported_reader_version") {
        const auto report = VerifySessionDir(session_dir);
        REQUIRE_FALSE(report.ok);
        REQUIRE(report.streams.size() == 1);
        CHECK(report.streams[0].error_code == "verify.unsupported_reader_version");
    }
    // 读过多口之后 Journal 一枚字节不改(§十七:回滚不改写现有 v1 Journal)。
    CHECK(ReadFileText(stream) == bytes_before);
    std::filesystem::remove_all(parent, ec);
}

TEST_CASE("回滚纪律: 超前 envelope schema_version 整本明拒") {
    std::vector<nlohmann::json> lines = LoadGoldenLines();
    for (nlohmann::json& line : lines) {
        line["schema_version"] = 3;
    }
    const auto parent = std::filesystem::temp_directory_path() / "lubancode-reader-gate-schema";
    std::error_code ec;
    std::filesystem::remove_all(parent, ec);
    const std::filesystem::path session_dir = WriteTempSession(Rechain(lines), parent, "s2");
    const std::filesystem::path stream = session_dir / "main.jsonl";
    const std::string bytes_before = ReadFileText(stream);

    const auto report = VerifySessionDir(session_dir);
    REQUIRE_FALSE(report.ok);
    REQUIRE(report.streams.size() == 1);
    CHECK(report.streams[0].error_code == "schema.unsupported_version");

    const auto fold = FoldStreamReplay(stream);
    CHECK_FALSE(fold.ok());

    CHECK(ReadFileText(stream) == bytes_before);
    std::filesystem::remove_all(parent, ec);
}
