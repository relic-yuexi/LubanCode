// BlobStore 测试:内容寻址布局、原子幂等、读回验 hash、内联上限配置。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "hooks/hash.hpp"
#include "trajectory/blob_store.hpp"

using namespace lubancode::trajectory;

namespace {

std::filesystem::path MakeTempDir(const char* tag) {
    const auto dir = std::filesystem::temp_directory_path() / ("lubancode-traj-blob-" +
                                                               std::string(tag));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

}  // namespace

TEST_CASE("blob: 内容寻址落位 sha256/<前2字符>/<全hash>") {
    const auto root = MakeTempDir("layout");
    BlobStore store(root);
    const std::string content = "hello trajectory";
    const auto ref = store.Store(content, "text/plain", Durability::ProcessCrash);
    REQUIRE(ref.has_value());
    const std::string sha = lubancode::hooks::Sha256Hex(content);
    CHECK(ref->sha256 == sha);
    CHECK(ref->size == content.size());
    CHECK(ref->media_type == "text/plain");
    CHECK(ref->encoding == "utf-8");
    CHECK(ref->compression == "none");
    const std::filesystem::path expected =
        root / "sha256" / sha.substr(0, 2) / sha;
    CHECK(std::filesystem::exists(expected));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("blob: 同内容幂等,不重写文件") {
    const auto root = MakeTempDir("idempotent");
    BlobStore store(root);
    const std::string content(5000, 'x');
    const auto first = store.Store(content, "text/plain", Durability::ProcessCrash);
    REQUIRE(first.has_value());
    const auto target = store.PathFor(first->sha256);
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(target, ec);
    const auto second = store.Store(content, "text/plain", Durability::ProcessCrash);
    REQUIRE(second.has_value());
    CHECK(second->sha256 == first->sha256);
    // 内容寻址:同 hash 只一份。
    CHECK(std::filesystem::last_write_time(target, ec) == stamp);
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("blob: ReadVerified 核 hash,坏内容拒供") {
    const auto root = MakeTempDir("verify");
    BlobStore store(root);
    const std::string content = "abc123";
    const auto ref = store.Store(content, "text/plain", Durability::PowerLoss);
    REQUIRE(ref.has_value());
    const auto back = store.ReadVerified(*ref);
    REQUIRE(back.has_value());
    CHECK(*back == content);

    // 篡改 blob 后必须拒。
    const auto path = store.PathFor(ref->sha256);
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "tampered";
    }
    CHECK_FALSE(store.ReadVerified(*ref).has_value());
    // 大小对不上也拒。
    BlobRef wrong_size = *ref;
    wrong_size.size += 1;
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("blob: BlobRef 往返与形状判定") {
    BlobRef ref;
    ref.sha256 = std::string(64, '0');
    ref.size = 42;
    ref.media_type = "application/json";
    const nlohmann::json json = ref.ToJson();
    CHECK(BlobRef::MatchesShape(json));
    const auto back = BlobRef::FromJson(json);
    REQUIRE(back.has_value());
    CHECK(back->sha256 == ref.sha256);
    CHECK(back->size == ref.size);
    CHECK(back->media_type == ref.media_type);

    CHECK_FALSE(BlobRef::MatchesShape(nlohmann::json{{"sha256", "short"}}));
    CHECK_FALSE(BlobRef::MatchesShape(nlohmann::json::object()));
    // 多一键即不是本形状。
    auto extra = json;
    extra["extra"] = 1;
    CHECK_FALSE(BlobRef::MatchesShape(extra));
}

TEST_CASE("blob: 内联上限可配") {
    const auto root = MakeTempDir("limit");
    BlobStoreOptions options;
    options.inline_limit = 16;
    BlobStore store(root, options);
    CHECK(store.inline_limit() == 16);
    const std::string inside(16, 'a');   // 恰好上限:内联
    const std::string beyond(17, 'b');   // 超一字:offload
    CHECK(inside.size() <= store.inline_limit());
    CHECK(beyond.size() > store.inline_limit());
    const auto ref = store.Store(beyond, "text/plain", Durability::Buffered);
    REQUIRE(ref.has_value());
    CHECK(ref->size == 17);
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("blob: PowerLoss 档走 fsync 路径不报错") {
    const auto root = MakeTempDir("powerloss");
    BlobStore store(root);
    const auto ref = store.Store(std::string(100000, 'z'), "text/plain", Durability::PowerLoss);
    REQUIRE(ref.has_value());
    CHECK(store.ReadVerified(*ref).has_value());
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}
