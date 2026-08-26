// @ 提及的会话支件(会话终章自大类搬出):文件索引(按 Git 根缓存)与
// 提交前校验/账单。索引按根缓存,根变了(cwd/worktree 切换)由 Invalidate
// 清缓存重扫;校验是纯函数(目标消失或跑出工作区要明报错)。
#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "cli/mention_menu.hpp"  // FileMentionEntry

namespace lubancode::app {

class MentionSupport {
public:
    // 文件索引快照:Git 根优先(提"项目文件"按项目走),没有根就 cwd。
    // 深度限 6、条目限 3000,排除 .git/构建产物/依赖目录/点目录。相对路径
    // 一律正斜杠。根没变就返回缓存。
    std::vector<lubancode::cli::FileMentionEntry> Snapshot();

    // 根变了(cwd/worktree 切换),清缓存。
    void Invalidate();

    // 提交前提及校验:目标消失或跑出工作区要明报错,这轮不发。活着的提及
    // 附一份"相对 → 绝对"账给模型(turn context,不进永久 history),不叫
    // 模型猜裸路径。图片路径不进账——它们走视觉附件路。
    // 返回:第一段是错误(非空 = 拦下这一轮不发送),第二段是给模型的提
    // 及账(空 = 没有)。
    std::pair<std::string, std::string> BuildLedger(const std::string& content);

private:
    std::vector<lubancode::cli::FileMentionEntry> mention_index_;
    std::string mention_index_root_;
};

}  // namespace lubancode::app
