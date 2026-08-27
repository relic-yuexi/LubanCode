// wire fixture loader 实现(模型协议兼容实录矩阵单,P0)。
// sha256 是一份紧凑自持实现(FIPS 180-4),不引新依赖——fixture 对账
// 只需要"手册字节 -> 摘要"可复现,不需要密码学强度之外的任何性能。

#include "api_fixture.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>

#include "api/sse_framing.hpp"
#include "platform/paths.hpp"  // Utf8ToWide:中文文件名不走 ACP 窄口

namespace lubancode_test {

namespace {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// sha256(公有域式实现,逐块 512 bit)
// ---------------------------------------------------------------------------

class Sha256 {
public:
    Sha256() {
        state_ = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                  0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    }

    void Update(const unsigned char* data, std::size_t length) {
        for (std::size_t i = 0; i < length; ++i) {
            buffer_[buffer_size_++] = data[i];
            if (buffer_size_ == 64) {
                Transform(buffer_.data());
                buffer_size_ = 0;
                bits_ += 512;
            }
        }
    }

    std::string HexDigest() {
        const std::uint64_t bits = bits_ + buffer_size_ * 8;
        unsigned char one = 0x80;
        Update(&one, 1);
        unsigned char zero = 0;
        while (buffer_size_ != 56) {
            Update(&zero, 1);
        }
        // 长度按大端 8 字节收尾(不再走 Update,免得 bits_ 被改)。
        unsigned char length_bytes[8];
        for (int i = 0; i < 8; ++i) {
            length_bytes[i] = static_cast<unsigned char>((bits >> (56 - i * 8)) & 0xFF);
        }
        for (const unsigned char byte : length_bytes) {
            buffer_[buffer_size_++] = byte;
        }
        Transform(buffer_.data());
        buffer_size_ = 0;
        std::ostringstream out;
        for (const std::uint32_t word : state_) {
            static const char* kHex = "0123456789abcdef";
            out << kHex[(word >> 28) & 0xF] << kHex[(word >> 24) & 0xF]
                << kHex[(word >> 20) & 0xF] << kHex[(word >> 16) & 0xF]
                << kHex[(word >> 12) & 0xF] << kHex[(word >> 8) & 0xF]
                << kHex[(word >> 4) & 0xF] << kHex[word & 0xF];
        }
        return out.str();
    }

private:
    static std::uint32_t Rotr(std::uint32_t value, int bits) {
        return (value >> bits) | (value << (32 - bits));
    }

    void Transform(const unsigned char* block) {
        static const std::uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
            0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
            0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
            0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
            0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
            0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
            0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
            0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
                   (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
                   static_cast<std::uint32_t>(block[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = h + s1 + ch + k[i] + w[i];
            const std::uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_;
    std::array<unsigned char, 64> buffer_{};
    std::size_t buffer_size_ = 0;
    std::uint64_t bits_ = 0;
};

std::optional<std::string> ReadFileBytes(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) return std::nullopt;
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return std::nullopt;
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// UTF-8 字节串 -> 平台路径。Windows 下过宽口(手册文件名带中文,窄口走
// ACP 打不开);POSIX 直接 utf-8。
fs::path Utf8Path(const std::string& utf8) {
#ifdef _WIN32
    return fs::path(lubancode::platform::Utf8ToWide(utf8));
#else
    return fs::path(utf8);
#endif
}

const std::string* RequiredString(const nlohmann::json& manifest, const char* field,
                                  const std::string& where, std::string* error) {
    const auto it = manifest.find(field);
    if (it == manifest.end() || !it->is_string() || it->get<std::string>().empty()) {
        *error = where + "." + field + " 必须是非空字符串";
        return nullptr;
    }
    return &it->get_ref<const std::string&>();
}

bool WireDirKnown(const std::string& wire_dir) {
    return wire_dir == "anthropic_messages" || wire_dir == "openai_chat" ||
           wire_dir == "openai_responses" || wire_dir == "google_generate_content";
}

}  // namespace

std::vector<std::pair<std::string, std::string>> ApiFixture::SseFrames() const {
    std::vector<std::pair<std::string, std::string>> frames;
    lubancode::api::SseFramer framer;
    // fixture 文件都以空行收尾,一整串喂进去即可切齐全部帧;尾部真有残句
    // (缺空行)就少一帧——那本身就是 fixture 坏了,回放测试比对时序会抓到。
    for (const auto& frame : framer.feed(stream)) {
        frames.emplace_back(frame.event, frame.data);
    }
    return frames;
}

std::string Sha256File(const std::filesystem::path& path) {
    const auto bytes = ReadFileBytes(path);
    if (!bytes.has_value()) return {};
    Sha256 digest;
    digest.Update(reinterpret_cast<const unsigned char*>(bytes->data()), bytes->size());
    return digest.HexDigest();
}

std::filesystem::path ManualPath(const std::string& filename) {
    return fs::path(LUBANCODE_SOURCE_DIR) / Utf8Path(filename);
}

const std::vector<std::string>& ManualSourceDocuments() {
    // 三份本地兼容手册(仓库根),fixture manifest 的 source_document 只认
    // 这三份(或 internal)。
    static const std::vector<std::string> kManuals = {
        "OpenAI兼容-Responses.md", "OpenAI兼容-Chat.md", "Anthropic兼容-Messages.md"};
    return kManuals;
}

std::expected<ApiFixture, std::string> LoadApiFixture(const std::string& wire_dir,
                                                      const std::string& id) {
    if (!WireDirKnown(wire_dir)) {
        return std::unexpected("api fixture 的 wire 目录不认识: " + wire_dir);
    }
    const fs::path base = fs::path(LUBANCODE_TEST_FIXTURES_DIR) / "api" / wire_dir;
    const std::string where = "fixtures/api/" + wire_dir + "/" + id;

    const auto stream = ReadFileBytes(base / (id + ".sse"));
    if (!stream.has_value() || stream->empty()) {
        return std::unexpected(where + ".sse 读不动或是空的");
    }
    const auto manifest_text = ReadFileBytes(base / (id + ".json"));
    if (!manifest_text.has_value()) {
        return std::unexpected(where + ".json manifest 缺失");
    }
    nlohmann::json manifest;
    try {
        manifest = nlohmann::json::parse(*manifest_text);
    } catch (const nlohmann::json::exception& e) {
        return std::unexpected(where + ".json 不是合法 JSON: " + e.what());
    }
    if (!manifest.is_object()) {
        return std::unexpected(where + ".json 顶层必须是 object");
    }

    ApiFixture fixture;
    fixture.stream = *stream;
    std::string error;
    const std::vector<std::pair<const char*, std::string*>> required = {
        {"fixture_id", &fixture.fixture_id}, {"wire", &fixture.wire},
        {"provider", &fixture.provider},     {"model", &fixture.model},
        {"scenario", &fixture.scenario},     {"source_document", &fixture.source_document},
        {"captured_at", &fixture.captured_at},
    };
    for (const auto& [field, target] : required) {
        const std::string* value = RequiredString(manifest, field, where, &error);
        if (value == nullptr) return std::unexpected(error);
        *target = *value;
    }

    if (fixture.fixture_id != id) {
        return std::unexpected(where + ".json 的 fixture_id(" + fixture.fixture_id +
                               ")与文件名不一致");
    }
    const std::string expected_wire = wire_dir == "anthropic_messages"
                                          ? "anthropic-messages"
                                          : wire_dir == "openai_chat"
                                                ? "openai-chat-completions"
                                                : wire_dir == "openai_responses"
                                                      ? "openai-responses"
                                                      : "google-generate-content";
    if (fixture.wire != expected_wire) {
        return std::unexpected(where + ".json 的 wire(" + fixture.wire + ")与目录不符(" +
                               expected_wire + ")");
    }

    const bool manual_source =
        std::find(ManualSourceDocuments().begin(), ManualSourceDocuments().end(),
                  fixture.source_document) != ManualSourceDocuments().end();
    if (fixture.source_document != "internal") {
        if (!manual_source) {
            return std::unexpected(where + ".source_document 不是三份手册之一,也不是 internal: " +
                                   fixture.source_document);
        }
        if (manifest.contains("doc_snapshot_hash") &&
            manifest["doc_snapshot_hash"].is_string()) {
            fixture.doc_snapshot_hash = manifest["doc_snapshot_hash"].get<std::string>();
        }
        if (fixture.doc_snapshot_hash.empty()) {
            return std::unexpected(where + ".doc_snapshot_hash 手册来源必填(sha256)");
        }
        const std::string* section = RequiredString(manifest, "source_section", where, &error);
        if (section == nullptr) return std::unexpected(error);
        fixture.source_section = *section;
    } else if (manifest.contains("doc_snapshot_hash")) {
        return std::unexpected(where + ".doc_snapshot_hash internal 来源不记手册 hash");
    }

    if (manifest.contains("request_expectation")) {
        fixture.request_expectation = manifest["request_expectation"];
    }
    if (manifest.contains("expected_events")) {
        if (!manifest["expected_events"].is_array()) {
            return std::unexpected(where + ".expected_events 必须是字符串数组");
        }
        for (const auto& event : manifest["expected_events"]) {
            if (!event.is_string() || event.get<std::string>().empty()) {
                return std::unexpected(where + ".expected_events 必须是非空字符串数组");
            }
            fixture.expected_events.push_back(event.get<std::string>());
        }
    }
    if (fixture.expected_events.empty()) {
        return std::unexpected(where + ".expected_events 至少记一桩(事件序列是 fixture 的账)");
    }
    if (manifest.contains("expected_replay")) {
        for (const auto& item : manifest["expected_replay"]) {
            if (!item.is_string()) {
                return std::unexpected(where + ".expected_replay 必须是字符串数组");
            }
            fixture.expected_replay.push_back(item.get<std::string>());
        }
    }
    if (manifest.contains("usage_expectation")) {
        fixture.usage_expectation = manifest["usage_expectation"];
    }
    if (manifest.contains("stop_reason")) {
        fixture.stop_reason = manifest["stop_reason"].get<std::string>();
    }
    if (manifest.contains("notes")) {
        fixture.notes = manifest["notes"].get<std::string>();
    }
    return fixture;
}

std::expected<std::vector<ApiFixture>, std::string> LoadAllApiFixtures() {
    std::vector<ApiFixture> all;
    std::vector<std::string> seen_ids;
    const fs::path root = fs::path(LUBANCODE_TEST_FIXTURES_DIR) / "api";
    std::error_code ec;
    if (!fs::exists(root, ec) || ec) {
        return std::unexpected("tests/fixtures/api/ 目录不存在");
    }
    for (const std::string wire_dir :
         {"anthropic_messages", "openai_chat", "openai_responses", "google_generate_content"}) {
        std::vector<fs::path> manifests;
        for (const auto& entry : fs::directory_iterator(root / wire_dir, ec)) {
            if (entry.path().extension() == ".json") manifests.push_back(entry.path());
        }
        if (ec) return std::unexpected("扫不动 fixtures/api/" + wire_dir + ": " + ec.message());
        std::sort(manifests.begin(), manifests.end());
        for (const auto& manifest_path : manifests) {
            auto fixture = LoadApiFixture(wire_dir, manifest_path.stem().string());
            if (!fixture.has_value()) return std::unexpected(fixture.error());
            if (std::find(seen_ids.begin(), seen_ids.end(), fixture->fixture_id) != seen_ids.end()) {
                return std::unexpected("fixture id 重复: " + fixture->fixture_id);
            }
            seen_ids.push_back(fixture->fixture_id);
            all.push_back(std::move(*fixture));
        }
    }
    return all;
}

}  // namespace lubancode_test
