// SV-09(2026-09-21 架构审查)拆出的主题存储(内部件):主题文件/catalog
// 的扫描与装载、写入校验、指纹、worker 落盘操作(upsert/forget/verify)、
// 编辑会话、schema 迁移与 index 重建。只认门面递进来的目录参数,不自行
// 解析身份,不持授权状态——授权闸都在 ProjectMemory 门面上,一份规则。

#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "memory/project_memory.hpp"

namespace lubancode::memory::store {

// catalog/主题文件的内部形状:公开条目 + 指纹表。
struct StoredEntry {
    MemoryEntry public_entry;
    nlohmann::json fingerprints = nlohmann::json::object();
};

// 扫描某层主题目录(facts/preferences/feedback;用户层无 facts)。同 id
// 撞车的两份都停成 conflict;读不动的记进 warnings。
std::vector<StoredEntry> ScanTopics(const std::filesystem::path& memory_dir,
                                    std::vector<std::string>* warnings = nullptr,
                                    const char* layer = "project");

// 装载 catalog.json;没有或坏了退回整目录扫描。
std::vector<StoredEntry> LoadCatalog(const std::filesystem::path& memory_dir,
                                     std::string* error = nullptr, const char* layer = "project");

// 指纹是否全部对得上当前项目文件(漂移 = 主题该核验了)。
bool FingerprintsCurrent(const StoredEntry& entry, const std::filesystem::path& project_root);

// 保存请求校验(门面 EnqueueSave 与候选审阅共用同一套:长度/敏感内容/
// 路径/scope/日期形状)。
std::expected<void, std::string> ValidateSaveRequest(const SaveRequest& request);

// 某层 catalog 里有没有这个 id(forget/verify 的层路由)。
bool LayerHasEntry(const std::filesystem::path& memory_dir, const std::string& id);

// worker 提交回执的四件套(合同 §四 memory.save.committed 的材料)。
struct MemoryWriteOutcome {
    std::string memory_id;
    std::string memory_path;     // UTF-8,workspace 内相对 memory 根
    std::string content_sha256;  // 写成文件后的正文指纹
    std::string committed_at;
};

// worker 侧落盘操作(ProcessJob 消费;job 是 pending/<id>.json 的原样)。
std::expected<MemoryWriteOutcome, std::string> ProcessUpsert(const nlohmann::json& job,
                                                             const std::filesystem::path& memory_dir,
                                                             const std::filesystem::path& project_root);
std::expected<void, std::string> ProcessForget(const nlohmann::json& job,
                                               const std::filesystem::path& memory_dir);
std::expected<void, std::string> ProcessVerify(const nlohmann::json& job,
                                               const std::filesystem::path& memory_dir,
                                               const std::filesystem::path& project_root);

// 按 id 在两层里找主题,返回 <条目, 所在目录>。show/open 共用;管理命令
// 不看召回授权(授权闸在门面)。
std::optional<std::pair<MemoryEntry, std::filesystem::path>> FindTopicAcrossLayers(
    const std::filesystem::path& memory_dir, const std::filesystem::path& user_memory_dir,
    const std::string& id);

// /memory show 的引擎:读整份主题文件。
std::expected<std::pair<std::string, std::filesystem::path>, std::string> ReadTopicForShow(
    const std::filesystem::path& memory_dir, const std::filesystem::path& user_memory_dir,
    const std::string& id);

// 编辑会话三段式:Begin 建同目录临时副本,Commit parse+校验(不得改 id
// 与层)后原子替换并重建该层派生物。测试直接用这对。
std::expected<ProjectMemory::TopicEditSession, std::string> BeginTopicEdit(
    const std::filesystem::path& memory_dir, const std::filesystem::path& user_memory_dir,
    const std::string& id);
std::expected<void, std::string> CommitTopicEdit(const ProjectMemory::TopicEditSession& session);

// 显式迁移(规格"迁移"):旧格式主题批迁 schema 3。先出计划不动盘;
// 确认后备份原件、原子写新档、重建派生物,中途失败回退。
ProjectMemory::MigrationPlan PlanSchemaMigration(const std::filesystem::path& memory_dir);
std::expected<ProjectMemory::MigrationResult, std::string> RunSchemaMigration(
    const std::filesystem::path& memory_dir);

}  // namespace lubancode::memory::store
