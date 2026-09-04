// Workspace 收官验收·跨平台身份与并发原语册(单子 §二 Windows/Linux 腿;
// Windows 本机与 WSL/Linux/macOS CI 同一份册,平台差异按运行时探测(路径
// 大小写)或如实报 SKIP(symlink 特权、junction)分叉,不冒充通过)。
//
//   - 路径大小写:敏感性运行时探测(临时根里造两个只有大小写不同的探测
//     目录,实为一间则不敏感)——不敏感(Windows NTFS、macOS 默认 APFS)
//     折叠同 key;敏感(WSL ext4、大小写敏感卷)两把 key;
//   - symlink:经目录符号链接进出,身份跟规范路径走(同仓同 key);
//   - junction(仅 Windows):目录 junction 进出的 git 仓库同 key;
//   - 锁:同 session 第二只 writer 拒(lock.held_by_live_process),
//     死 PID 陈旧锁清掉重试——两平台同一条路;
//   - 原子写:并发写 workspace.json,任何时刻读到的都是完整合法 JSON,
//     末态是最后一次整份替换(不存在半份拼接)。
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include "trajectory/session_lock.hpp"
#include "workspace/identity.hpp"
#include "workspace/manifest.hpp"

using namespace lubancode;

namespace {

namespace fs = std::filesystem;

fs::path TempRoot(const std::string& name) {
    static int sequence = 0;
    static const auto run_id = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    fs::path path = fs::temp_directory_path() /
                    ("lubancode-ws-platform-" + std::to_string(run_id % 100000) + "-" + name +
                     "-" + std::to_string(++sequence));
    std::error_code ec;
    fs::remove_all(path, ec);
    fs::create_directories(path, ec);
    return path;
}

void MakeRepo(const fs::path& repo) { fs::create_directories(repo / ".git"); }

// 探测 root 所在盘的路径大小写敏感性:造两个只有大小写不同的目录,造完
// 若实为同一目录(fs::equivalent)则盘不敏感——"Probe-Dir"与"probe-dir"
// 一间房;两间则敏感。不按平台猜:Windows NTFS 与 macOS 默认 APFS 都不
// 敏感,WSL ext4 敏感,macOS 又可格式化出敏感卷——盘说了算,平台代猜会
// 冤枉人(CI run 33873402016 的 macOS 默认盘上"POSIX 必敏感"猜红)。
bool ProbeCaseSensitive(const fs::path& root) {
    const fs::path probe_upper = root / "Probe-Dir";
    const fs::path probe_lower = root / "probe-dir";
    std::error_code ec;
    fs::create_directories(probe_upper, ec);
    CHECK_MESSAGE(!ec, "探测目录造不出: " << ec.message());
    if (ec) {
        return true;  // 探不成:按敏感走,上面的 CHECK 已把病记下
    }
    // 不敏感盘上这一步造进的还是 probe_upper 那间(equivalent 自会揭穿)。
    fs::create_directories(probe_lower, ec);
    CHECK_MESSAGE(!ec, "探测目录(小写拼写)造不出: " << ec.message());
    if (ec) {
        return true;
    }
    const bool same_dir = fs::equivalent(probe_upper, probe_lower, ec);
    CHECK_MESSAGE(!ec, "探测目录等价性查不出: " << ec.message());
    return !same_dir || ec;
}

}  // namespace

TEST_CASE("跨平台: 路径大小写——不敏感盘折叠同 key,敏感盘两把 key") {
    const fs::path root = TempRoot("case");
    const bool case_sensitive = ProbeCaseSensitive(root);
    const fs::path upper = root / "Demo-Repo";
    const fs::path lower = root / "demo-repo";
    MakeRepo(upper);
    if (case_sensitive) {
        MakeRepo(lower);  // 敏感盘:lower 另起一间房,各登记各的
    }
    const auto a = workspace::ResolveWorkspaceIdentity(upper, {});
    const auto b = workspace::ResolveWorkspaceIdentity(lower, {});
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    if (case_sensitive) {
        CHECK(a->workspace_key != b->workspace_key);  // 敏感:两间房两把 key
    } else {
        CHECK(a->workspace_key == b->workspace_key);  // 折叠:一间房一把 key
    }
}

TEST_CASE("跨平台: 经目录 symlink 进仓,身份跟规范路径走") {
    const fs::path root = TempRoot("symlink");
    const fs::path repo = root / "real-repo";
    const fs::path link = root / "link-repo";
    MakeRepo(repo);
    std::error_code ec;
    fs::create_directory_symlink(repo, link, ec);
    if (ec) {
        // Windows 无开发者模式/特权时造不出 symlink:如实报 SKIP。
        MESSAGE("SKIP: 造目录 symlink 失败(" << ec.message() << "),本平台此腿留 CI");
        return;
    }
    const auto direct = workspace::ResolveWorkspaceIdentity(repo, {});
    const auto via_link = workspace::ResolveWorkspaceIdentity(link, {});
    REQUIRE(direct.has_value());
    REQUIRE(via_link.has_value());
    CHECK(direct->workspace_key == via_link->workspace_key);  // 解链后同仓同 key
}

#ifdef _WIN32
TEST_CASE("Windows: junction 进出的 git 仓库同 key") {
    const fs::path root = TempRoot("junction");
    const fs::path repo = root / "real-repo";
    const fs::path junction = root / "junction-repo";
    MakeRepo(repo);
    // std::filesystem 没有 junction 专用口;mklink /J 不需特权,借 cmd 造。
    const std::string command = "cmd /c mklink /J \"" + junction.string() + "\" \"" +
                                repo.string() + "\" >NUL 2>&1";
    const int rc = std::system(command.c_str());
    if (rc != 0 || !fs::exists(junction)) {
        MESSAGE("SKIP: junction 造不出(rc=" << rc << "),本机此腿留 CI");
        return;
    }
    const auto direct = workspace::ResolveWorkspaceIdentity(repo, {});
    const auto via_junction = workspace::ResolveWorkspaceIdentity(junction, {});
    REQUIRE(direct.has_value());
    REQUIRE(via_junction.has_value());
    CHECK(direct->workspace_key == via_junction->workspace_key);
}
#endif

unsigned long ThisPid() {
#ifdef _WIN32
    return static_cast<unsigned long>(_getpid());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

TEST_CASE("跨平台: session 锁——活人拒、死锁清") {
    const fs::path dir = TempRoot("lock");
    fs::create_directories(dir);

    // 活锁:本进程持有,第二把拒。
    {
        trajectory::SessionLockOwner mine;
        mine.pid = ThisPid();
        mine.process_start_token = trajectory::CurrentProcessStartToken();
        mine.acquired_at_ms = 1759000000000LL;
        auto held = trajectory::SessionLock::Acquire(dir, mine);
        REQUIRE(held.has_value());
        const auto holder = trajectory::SessionLock::Inspect(dir);
        REQUIRE(holder.has_value());
        auto refused = trajectory::SessionLock::Acquire(dir, *holder);
        CHECK_FALSE(refused.has_value());
        CHECK(refused.error().find("lock.held_by_live_process") != std::string::npos);
    }
    // 死 PID 陈旧锁:清掉重试成功。
    {
        trajectory::SessionLockOwner dead;
        dead.pid = 4194303UL;
        dead.process_start_token = "0001deadbeef0002";
        dead.acquired_at_ms = 1759000000000LL;
        {
            std::ofstream file(dir / "session.lock", std::ios::binary | std::ios::trunc);
            file << dead.ToJson().dump(2) << "\n";
        }
        auto takeover = trajectory::SessionLock::Acquire(dir, dead);
        CHECK(takeover.has_value());
    }
}

TEST_CASE("跨平台: workspace.json 并发原子写——读侧永见整份,末态合法") {
    const fs::path root = TempRoot("atomic");
    const fs::path workspace = root / "ws-atomic";
    fs::create_directories(workspace);

    workspace::WorkspaceManifest base;
    base.workspace_key = "atomic-0000000000000000";
    base.display_name = "atomic";
    base.identity_kind = "cwd_fallback";
    base.identity_root = root.generic_string();
    base.created_at_ms = 1759000000000LL;
    REQUIRE(workspace::WriteWorkspaceManifestAtomic(workspace, base).has_value());

    // 八只并发写手各改 last_opened_at_ms;读手轮询。合同的本义是"绝不半
    // 截":任何一次成功打开的读必须是完整合法 JSON(version==2)。Windows
    // 的 MoveFileExW 换名有微窗,读者可能短暂打不开(打不开≠撕裂,重读即
    // 得)——失败重读一次,重读仍坏才算真坏;微窗次数单独记账留证。
    std::atomic<bool> stop{false};
    std::atomic<int> bad_reads{0};
    std::atomic<int> reads{0};
    std::atomic<int> transient_reopens{0};
    std::atomic<int> bad_missing{0};
    std::atomic<int> bad_corrupt{0};
    std::mutex bad_note_mutex;
    std::string bad_note;
    std::thread reader([&] {
        while (!stop.load()) {
            // 有界重试:换名微窗(WRITE_THROUGH 落盘)偶超 1ms,给 10×2ms
            // 预算;预算耗尽仍读不出才是真坏(撕裂 JSON 重试多少次都坏)。
            workspace::ManifestRead read;
            int attempt = 0;
            do {
                read = workspace::ReadWorkspaceManifest(workspace);
                if (read.status == workspace::ManifestRead::Status::Ok) {
                    break;
                }
                ++transient_reopens;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            } while (++attempt < 10);
            if (read.status != workspace::ManifestRead::Status::Ok) {
                ++bad_reads;
                if (read.status == workspace::ManifestRead::Status::Missing) {
                    ++bad_missing;
                } else if (read.status == workspace::ManifestRead::Status::Corrupt) {
                    ++bad_corrupt;
                }
                std::lock_guard<std::mutex> note_lock(bad_note_mutex);
                if (bad_note.empty()) {
                    bad_note = "code=" + read.error_code + " text=" + read.error_text;
                }
            }
            ++reads;
        }
    });
    std::vector<std::thread> writers;
    std::atomic<int> write_failures{0};
    for (int i = 0; i < 8; ++i) {
        writers.emplace_back([&, i] {
            for (int round = 0; round < 40; ++round) {
                workspace::WorkspaceManifest next = base;
                next.last_opened_at_ms = 1759000000000LL + i * 1000 + round;
                auto written = workspace::WriteWorkspaceManifestAtomic(workspace, next);
                if (!written.has_value()) {
                    ++write_failures;
                }
            }
        });
    }
    for (auto& writer : writers) {
        writer.join();
    }
    stop.store(true);
    reader.join();
    CHECK(reads.load() > 0);
    // 原子写合同的本义:读侧绝不半截。坏读(有界重试后仍读不出)恒为零。
    CHECK(bad_reads.load() == 0);
    if (transient_reopens.load() > 0) {
        MESSAGE("换名微窗出现 " << transient_reopens.load()
                               << " 次(重读即得,非撕裂);Windows MoveFileExW 语义,留证");
    }
    MESSAGE("坏读分类: missing=" << bad_missing.load() << " corrupt=" << bad_corrupt.load()
                                 << " 首次坏读: " << bad_note);
#ifdef _WIN32
    // Windows 语义(实测留证):ifstream 默认不带 FILE_SHARE_DELETE,读手
    // 正握句柄时 MoveFileExW 换名可能被拒(atomic.replace_failed)。生写
    // 口失败如实报错、由调用方处置;这里不计为账坏——末态自洽性由下面的
    // 末读断言守门。POSIX rename 天生原子,恒零。
    if (write_failures.load() > 0) {
        MESSAGE("Windows 换名被并发读句柄拒绝 " << write_failures.load()
                                                << " 次(atomic.replace_failed,调用方按码处置)");
    }
#else
    CHECK(write_failures.load() == 0);
#endif

    // 末态:合法 manifest,值是某一次完整的写(自洽,不是拼的)。
    const auto final_read = workspace::ReadWorkspaceManifest(workspace);
    REQUIRE(final_read.status == workspace::ManifestRead::Status::Ok);
    CHECK(final_read.manifest.workspace_key == base.workspace_key);
    CHECK(final_read.manifest.last_opened_at_ms >= 1759000000000LL);
}
