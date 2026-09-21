// SV-09(2026-09-21 架构审查)拆出的召回引擎(内部件):词法(归一化/
// 双路分词/正文词袋)、排级、相关性分级、trace 落盘与注入材料拼装。
// 纯函数群——身份、目录、授权、扩展词、落账口全部由 ProjectMemory 门面
// 按调用递进来,本引擎不自行解析身份,也不自带授权状态。公共的
// RankEntries/GradeRelevance/TokenizeForRetrieval/NormalizeForRetrieval
// 声明在 project_memory.hpp(消费方零改动),实现住 recall_engine.cpp。

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "memory/project_memory.hpp"

namespace lubancode::memory::recall {

// 正文词袋构建("term:count ..." 空格分隔):分词与查询同款双路手艺。
// catalog 写侧(topic_store)与排级读侧共用这一份。
std::string BuildContentIndexBag(const std::string& content);

// expires_at 是否已过(格式宽松,字典序即时间序)。
bool EntryExpired(const MemoryEntry& entry);

// 读 .state/recall-traces/trace-last.json(/memory why 用)。没有记录时
// valid=false。
RecallTrace ReadRecallTrace(const std::filesystem::path& memory_dir);

// BuildTurnContext 的实现体(SV-09 从 ProjectMemory::BuildTurnContextImpl
// 搬来,逻辑一字未动)。授权闸、合成事件隔离、两层合并、预算选条、弱档
// 垫批、时间线拼装全在这;参数多正是边界——一块状态一份输入,门面是唯一
// 递水人:
//   options           本场授权与预算(global_allowed/enabled/use/user_enabled/
//                     learn/max_retrieval_bytes/max_results)
//   identity          workspace_key(落 trace)与 project_root(指纹对照)
//   memory_dir        项目层记忆目录
//   user_memory_dir   用户层记忆目录
//   retrieval_hints   回合总结产出的检索扩展词(learn off/失败时为空)
//   accounting        落账口(空 = 没接轨迹的场合,一笔不落)
// 其余为单次调用参数(query/cwd/origin/force_retrieval/target_run_id/turn_id)。
std::string BuildTurnContext(const Options& options, const ProjectIdentity& identity,
                             const std::filesystem::path& memory_dir,
                             const std::filesystem::path& user_memory_dir,
                             const std::vector<std::string>& retrieval_hints,
                             MemoryAccounting* accounting, const std::string& query,
                             const std::filesystem::path& cwd, QueryOrigin origin,
                             bool force_retrieval, const std::string& target_run_id,
                             const std::string& turn_id);

}  // namespace lubancode::memory::recall
