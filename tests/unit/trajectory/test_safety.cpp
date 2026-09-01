// P0-4 §12.1 文件权限与路径安全:单段名校验(拒绝 ../绝对路径/设备名)、
// canonical containment、reparse/symlink 防逃逸、user-only 收紧
//(POSIX 0700/0600,Windows PROTECTED user-only DACL)。加 SessionManager
// 用户递 session ref 的单段门(resume/archive/delete 不得被路径逃逸)。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "trajectory/safety.hpp"
#include "trajectory/session_manager.hpp"

using namespace lubancode::trajectory;

namespace {

std::filesystem::path FreshDir(const std::string& name) {
    const auto dir = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void PutFile(const std::filesystem::path& path, const std::string& content) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    out << content;
}

}  // namespace

TEST_CASE("IsSafeSingleSegment:单段名门") {
    CHECK(IsSafeSingleSegment("20260831-120000-AAAAAA"));
    CHECK(IsSafeSingleSegment("demo-0123456789ab"));
    CHECK(IsSafeSingleSegment("record-1"));

    CHECK_FALSE(IsSafeSingleSegment(""));            // 空
    CHECK_FALSE(IsSafeSingleSegment("."));           // 当前目录
    CHECK_FALSE(IsSafeSingleSegment(".."));          // 上跳
    CHECK_FALSE(IsSafeSingleSegment("../escape"));   // 路径
    CHECK_FALSE(IsSafeSingleSegment("a/b"));         // 分隔符
    CHECK_FALSE(IsSafeSingleSegment("a\\b"));        // Windows 分隔符
    CHECK_FALSE(IsSafeSingleSegment("C:thing"));     // 盘符
    CHECK_FALSE(IsSafeSingleSegment(".hidden"));     // 点打头
    CHECK_FALSE(IsSafeSingleSegment("con"));         // Windows 设备名
    CHECK_FALSE(IsSafeSingleSegment("NUL"));
    CHECK_FALSE(IsSafeSingleSegment("a b"));         // 空白
    CHECK_FALSE(IsSafeSingleSegment(std::string(200, 'x')));  // 超长
}

TEST_CASE("IsContainedCanonicalPath + IsSafeContainedPath:包含域与逃逸") {
    const auto root = FreshDir("lubancode-p4-safety");
    const auto session = root / "sessions/20260831-120000-AAAAAA";
    std::error_code ec;
    std::filesystem::create_directories(session, ec);

    CHECK(IsContainedCanonicalPath(session / "main.jsonl", session));
    // 尚不存在的子路径也能判包含(canonical 会补齐已存在的祖先)。
    CHECK(IsContainedCanonicalPath(session / "artifacts/sha256/ab/deadbeef", session));
    CHECK_FALSE(IsContainedCanonicalPath(session / ".." / "steal.json", session));
    CHECK_FALSE(IsContainedCanonicalPath(session, session));  // 同一路径不算"之下"
    CHECK(IsSafeContainedPath(session / "main.jsonl", session));

    // symlink 防逃逸:真造一枚指向域外的链,夹在 root 到 child 的路上就
    // 拦(造不出 symlink 的环境跳过——POSIX 与开了开发者模式的 Windows
    // 都造得出)。
    const auto outside = FreshDir("lubancode-p4-safety-outside");
    PutFile(outside / "secret.json", "{}");
    const auto link = session / "link-to-outside";
    std::error_code link_ec;
    std::filesystem::remove(link, link_ec);
    std::filesystem::create_directory_symlink(outside, link, link_ec);
    if (!link_ec && std::filesystem::is_directory(link)) {
        CHECK(ContainsSymlinkOrReparse(session, link / "secret.json"));
        CHECK_FALSE(IsSafeContainedPath(link / "secret.json", session));
    }
    // 干净路径:不含重解析点。
    CHECK_FALSE(ContainsSymlinkOrReparse(session, session / "main.jsonl"));
}

TEST_CASE("HardenDirectoryUserOnly / HardenFileUserOnly:user-only 收紧") {
    const auto root = FreshDir("lubancode-p4-harden");
    const auto dir = root / "trajectories/workspaces/demo/sessions";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto file = dir / "session.json";
    PutFile(file, "{}");

    CHECK(HardenDirectoryUserOnly(dir));
    CHECK(HardenFileUserOnly(file));
#ifndef _WIN32
    // POSIX:直接核 mode 位(0700/0600)。
    struct ::stat st {};
    REQUIRE(::stat(dir.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0700);
    REQUIRE(::stat(file.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);
#endif
    // 不存在的对象:收紧失败(不装成功)。
    CHECK_FALSE(HardenDirectoryUserOnly(root / "no-such-dir"));
    CHECK_FALSE(HardenFileUserOnly(root / "no-such-file"));
}

TEST_CASE("SessionManager:用户递的 session ref 过不了单段门就拒") {
    const auto root = FreshDir("lubancode-p4-ref-gate");
    SessionManagerOptions options;
    options.workspaces_root = root / "workspaces";
    options.workspace_root = root / "repo";
    options.lubancode_version = "test";
    std::error_code ec;
    std::filesystem::create_directories(root / "repo", ec);
    SessionManager manager(options);

    // resume 的 source ref:路径逃逸当场拒,不碰盘。
    ResumeRequest bad_resume;
    bad_resume.source_session_id = "../../etc";
    const auto resumed = manager.ResumeAsNew(bad_resume);
    CHECK(resumed.error_code == "resume.source_invalid_ref");

    // archive/delete 的目录管理口同门。
    CHECK(manager.ArchiveSession("..\\..\\windows").error() == "session.invalid_ref: session id 须是单段名(不带路径)");
    CHECK(manager.DeleteSession("../../home", "test").error() ==
          "session.invalid_ref: session id 须是单段名(不带路径)");

    // 合法单段名照旧走老路(找不到给 not_found,不是 invalid_ref)。
    CHECK(manager.ArchiveSession("20260831-000000-AAAAAA").error() != "session.invalid_ref: session id 须是单段名(不带路径)");
}
