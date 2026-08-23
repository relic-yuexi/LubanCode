// Ctrl+R 提问历史反向搜索(交互抛光总账第二批)的纯逻辑层:索引构建、
// 范围过滤、查询匹配、搜索框状态机与结果行渲染。终端(console_input)只
// 管把键喂进来、把行画出去;数据由应用层(interactive_session)只读
// session 事件账现抽——工具结果、密钥、未发送草稿压根不在账里,天然混
// 不进来(见 agent/session_store.hpp 的 ExtractPromptHistory)。
//
// 纯逻辑:不碰终端、不碰磁盘,tests/unit/cli/test_history_search.cpp 钉死。
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace lubancode::cli {

// 一条提问历史。
struct PromptHistoryEntry {
    std::string text;     // 完整提问(多行拼 '\n';展示用首行)
    std::string ts;       // "yyyy-mm-dd HH:MM:SS"(存档行里的原始 ts)
    std::string session_id;
    std::string title;    // 会话标题(可空,展示层退回首句)
    std::string project;  // 项目短名(cwd 末段;全部范围里分场用)
    std::string project_key;  // cwd 归一化比较键(范围过滤用)
};

// 应用层给的整份数据(一次搜索会话里取一次,范围轮换本地过滤)。
struct PromptHistoryDataset {
    std::vector<PromptHistoryEntry> entries;  // 时间序,旧→新
    std::string current_session_id;            // 本场(范围"本会话"用)
    std::string current_project_key;           // 本项目 cwd 归一化键
};

enum class HistorySearchScope { Session, Project, All };

// 范围轮换:本会话 -> 本项目 -> 全部 -> 本会话。
HistorySearchScope NextHistorySearchScope(HistorySearchScope scope);

// 数据集 + 范围 -> 搜索索引:过滤、连续同文去重、最新在前,截最近 max 条。
std::vector<PromptHistoryEntry> BuildHistorySearchIndex(const PromptHistoryDataset& dataset,
                                                        HistorySearchScope scope, std::size_t max = 500);

// 大小写不敏感的子串查询(ASCII 折小写;中文原样比)。返回索引里的命中
// 下标,最新优先;空查询 = 全部(仍是最新优先)。limit 截条数。
std::vector<std::size_t> SearchHistoryEntries(const std::vector<PromptHistoryEntry>& index,
                                              const std::string& query, std::size_t limit = 8);

// 搜索框状态机:查询在编辑器缓冲里(终端层维护),这里只记"结果集/选中
// 位/范围"。Move 永不越界;查询变了由终端层调 ResetMatches 重新查。
class HistorySearchSession {
public:
    void Open(PromptHistoryDataset dataset, HistorySearchScope initial_scope);
    bool active() const { return active_; }
    void Close() { active_ = false; }

    HistorySearchScope scope() const { return scope_; }
    // Ctrl+S:轮换范围并按新范围重建索引(查询重跑由调用方做)。
    void CycleScope();
    // 查询变化后重跑:matches 重建,选中位回到最新一条(0)。
    void Rerun(const std::string& query);

    const std::vector<PromptHistoryEntry>& index() const { return index_; }
    const std::vector<std::size_t>& matches() const { return matches_; }
    std::size_t selected() const { return selected_; }
    // Ctrl+R/↑:往更早一条;↓:往更新一条。到头停,不回绕。
    void MoveOlder();
    void MoveNewer();
    // 当前选中条目(没有命中给 nullopt)。
    const PromptHistoryEntry* SelectedEntry() const;

private:
    bool active_ = false;
    HistorySearchScope scope_ = HistorySearchScope::Session;
    PromptHistoryDataset dataset_;
    std::vector<PromptHistoryEntry> index_;
    std::vector<std::size_t> matches_;
    std::size_t selected_ = 0;
};

// 搜索框的整块显示行(纯文本,不夹 ANSI;宽度截断归 TruncateUtf8ToDisplayWidth,
// 由调用方在画的时候做)。首行是操作提示(含范围名与可用键),随后每条命中
// 一行:"❯ 首行文本… · ts · 标题/项目"。选中行前缀 "❯ ",其余 "  "。
// highlight_stats/highlight_reset 非空时把命中片段包上颜色(plain 主题传空串,
// 输出便无 ANSI——管道/重定向场景不添转义)。
std::vector<std::string> BuildHistorySearchLines(const HistorySearchSession& session,
                                                 const std::string& query, int width,
                                                 const std::string& highlight_stats,
                                                 const std::string& highlight_reset);

}  // namespace lubancode::cli
