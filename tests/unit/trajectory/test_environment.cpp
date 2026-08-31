// P0-4 环境快照与重现等级(§9.1/§9.2):GatherGitStatus 的真 IO 档在
// test_evidence_side_effects 里走过(非 git 仓降档);这里钉纯函数面——
// DetermineReplayLevel 的四档推导与 BuildEnvironmentCapturePayload 的
// 输出形状(大 dirty patch/untracked 落 blob、事件 payload 三键)。
#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "trajectory/blob_store.hpp"
#include "trajectory/environment.hpp"

using namespace lubancode::trajectory;

namespace {

std::filesystem::path FreshDir(const std::string& name) {
    const auto dir = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

EnvironmentSnapshotInput BaseInput() {
    EnvironmentSnapshotInput input;
    input.lubancode_version = "0.26.148";
    input.os_name = "windows";
    input.arch = "x86_64";
    input.locale = "C";
    input.timezone = "Asia/Shanghai";
    input.cwd = "D:/work/demo";
    input.repository_root = "D:/work/demo";
    input.git.in_repo = true;
    input.git.head_sha = std::string(64, 'a');
    input.git.branch = "main";
    input.git.dirty = false;
    input.provider = "demo";
    input.wire = "responses";
    input.model = "demo-large";
    input.toolset.toolset_sha256 = std::string(64, 'b');
    input.toolset.tool_count = 42;
    input.system_prompt_ref = BlobRef{std::string(64, 'c'), 9, "text/markdown", "utf-8", "none"};
    return input;
}

}  // namespace

TEST_CASE("DetermineReplayLevel:两轴齐全才 exact,缺一降一档") {
    // 两轴齐全:exact,零缺口。
    const auto full = BaseInput();
    const auto exact = DetermineReplayLevel(full);
    CHECK(exact.level == ReplayLevel::Exact);
    CHECK(exact.gaps.empty());

    // source 齐、环境缺(system prompt 没落):source_exact_environment_partial。
    auto partial = BaseInput();
    partial.system_prompt_ref.reset();
    const auto source_only = DetermineReplayLevel(partial);
    CHECK(source_only.level == ReplayLevel::SourceExactEnvironmentPartial);
    CHECK(std::find(source_only.gaps.begin(), source_only.gaps.end(),
                    "system_prompt_ref_unavailable") != source_only.gaps.end());

    // source 缺(脏仓没拿到 patch):环境再齐也顶多 input_only 之上不冒。
    auto dirty_no_patch = BaseInput();
    dirty_no_patch.git.dirty = true;
    dirty_no_patch.git.dirty_patch.clear();
    const auto dirty = DetermineReplayLevel(dirty_no_patch);
    CHECK(dirty.level == ReplayLevel::InputOnly);
    CHECK(std::find(dirty.gaps.begin(), dirty.gaps.end(), "dirty_patch_unavailable") !=
          dirty.gaps.end());

    // 不在仓里:source 轴缺,有 cwd 判 input_only。
    auto no_repo = BaseInput();
    no_repo.git.in_repo = false;
    no_repo.git.head_sha.clear();
    const auto outside = DetermineReplayLevel(no_repo);
    CHECK(outside.level == ReplayLevel::InputOnly);
    CHECK(std::find(outside.gaps.begin(), outside.gaps.end(), "not_a_git_repository") !=
          outside.gaps.end());

    // 连 cwd 都没有:blocked(防御性兜底)。
    auto nothing = BaseInput();
    nothing.cwd.clear();
    nothing.git.in_repo = false;
    nothing.git.head_sha.clear();
    nothing.lubancode_version.clear();
    nothing.toolset.toolset_sha256.clear();
    nothing.system_prompt_ref.reset();
    const auto blocked = DetermineReplayLevel(nothing);
    CHECK(blocked.level == ReplayLevel::Blocked);
    CHECK(std::find(blocked.gaps.begin(), blocked.gaps.end(), "cwd_unavailable") !=
          blocked.gaps.end());
}

TEST_CASE("BuildEnvironmentCapturePayload:大 diff/untracked 落 blob,payload 只带三键") {
    const auto dir = FreshDir("lubancode-p4-env-payload");
    BlobStore blobs(dir / "artifacts");

    auto input = BaseInput();
    input.git.dirty = true;
    // 40 KiB 的脏 diff:超默认 32 KiB 内联上限,快照里只留 blob 引用。
    input.git.dirty_patch = std::string(40 * 1024, '+');
    input.git.untracked_files = {"a.txt", "b.log"};

    const auto payload = BuildEnvironmentCapturePayload(input, blobs, Durability::PowerLoss);
    REQUIRE(payload.has_value());

    // 事件 payload 三键(schema 钉死),不含 diff 正文。
    const auto& event = payload->event_payload;
    REQUIRE(event.contains("snapshot_ref"));
    CHECK(event.value("replay_level", std::string()) == "exact");
    CHECK(event.contains("gaps"));

    // 快照本体:git 段带 dirty_patch_ref 与 untracked_manifest_ref,两份 blob
    // 都真落了盘、读得回。
    const auto& snapshot = payload->full_snapshot;
    REQUIRE(snapshot["git"].contains("dirty_patch_ref"));
    REQUIRE(snapshot["git"].contains("untracked_manifest_ref"));
    const auto patch_ref = BlobRef::FromJson(snapshot["git"]["dirty_patch_ref"]);
    REQUIRE(patch_ref.has_value());
    const auto patch = blobs.ReadVerified(*patch_ref);
    REQUIRE(patch.has_value());
    CHECK(patch->size() == 40 * 1024);
    const auto manifest_ref = BlobRef::FromJson(snapshot["git"]["untracked_manifest_ref"]);
    REQUIRE(manifest_ref.has_value());
    const auto manifest = blobs.ReadVerified(*manifest_ref);
    REQUIRE(manifest.has_value());
    const auto parsed_manifest = nlohmann::json::parse(*manifest, nullptr, false);
    REQUIRE_FALSE(parsed_manifest.is_discarded());
    CHECK(parsed_manifest.size() == 2);

    // §9.1 全字段都在快照里(版本/OS/身份/工具集/配置/env allowlist)。
    CHECK(snapshot["lubancode_version"] == "0.26.148");
    CHECK(snapshot["os"] == "windows");
    CHECK(snapshot["arch"] == "x86_64");
    CHECK(snapshot["locale"] == "C");
    CHECK(snapshot["timezone"] == "Asia/Shanghai");
    CHECK(snapshot["cwd"] == "D:/work/demo");
    CHECK(snapshot["provider"] == "demo");
    CHECK(snapshot["wire"] == "responses");
    CHECK(snapshot["model"] == "demo-large");
    CHECK(snapshot["toolset"]["tool_count"] == 42);
    CHECK(snapshot.contains("system_prompt_ref"));
    CHECK(snapshot.contains("config_snapshot_redacted"));
    CHECK(snapshot.contains("env_allowlist"));
}

TEST_CASE("GatherGitStatus:空根与不存在仓如实报 in_repo=false") {
    CHECK_FALSE(GatherGitStatus("").in_repo);
    CHECK_FALSE(GatherGitStatus("Z:/definitely/not/a/repo/here").in_repo);
}
