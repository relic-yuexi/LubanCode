// SV-09(2026-09-21 架构审查)拆出的候选审阅箱(内部件):待审候选住
// <memory_dir>/memory-candidates/<id>.json,原子替换;拒绝账本 rejected.json
// 只存短哈希与理由,不存被拒正文。授权闸不在箱上——generate_enabled 的
// 判定与 accept 的入队编排在 ProjectMemory 门面,规则一份。

#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "memory/project_memory.hpp"  // MemoryCandidate 合同

namespace lubancode::memory::candidates {

// 审阅箱目录(memory_dir/memory-candidates)。
std::filesystem::path CandidatesDir(const std::filesystem::path& memory_dir);

// 待审候选列表,按创建时间升序;坏候选文件跳过,不拦列表。
std::vector<MemoryCandidate> ListCandidates(const std::filesystem::path& memory_dir);
std::optional<MemoryCandidate> GetCandidate(const std::filesystem::path& memory_dir,
                                            const std::string& id);

// 新候选入库(待审区)。字段过与正式保存同一套校验;kind+标题查重,与
// 现有待审候选同主题时原位更新;此前被拒过的同主题候选直接拒收(短哈希
// 账本挡死缠烂打)。返回候选 id;被挡时返回错误。
std::expected<std::string, std::string> AddCandidate(const std::filesystem::path& memory_dir,
                                                     MemoryCandidate candidate);

// 改标题/正文后仍留待审区(校验同保存)。
std::expected<void, std::string> EditCandidate(const std::filesystem::path& memory_dir,
                                               const std::string& id, const std::string& title,
                                               const std::string& content);

// 拒绝:删候选与同主题残留,只留短哈希与理由进 rejected 账本。
std::expected<void, std::string> RejectCandidate(const std::filesystem::path& memory_dir,
                                                 const std::string& id, std::string reason);

// accept 排队成功后删候选文件(入队编排在门面)。
void RemoveCandidateFile(const std::filesystem::path& memory_dir, const std::string& id);

}  // namespace lubancode::memory::candidates
