// SessionTitleAccount 的实现(骨架拆解反弹·问题 2):判断逻辑自
// TerminalSessionController 的四个标题方法逐字搬来,行为一字未改;打印、
// 模型路由、peer 同步留 controller。
#include "app/session_title_account.hpp"

#include <utility>

#include "app/session_title.hpp"

namespace lubancode::app {

SessionTitleAccount::SessionTitleAccount(std::string& title,
                                         lubancode::runtime::TrajectorySessionLedger* ledger)
    : title_(title), ledger_(ledger) {}

// P0-2:账本在,标题真账就是 control.title.changed;"档子活没活"看
// ledger 的 main recorder。
bool SessionTitleAccount::LedgerActive() const {
    return ledger_ != nullptr && ledger_->main() != nullptr;
}

bool SessionTitleAccount::AppendTitleEvent(const std::string& title) {
    if (ledger_ != nullptr) {
        if (!LedgerActive()) {
            return false;
        }
        std::string old;
        if (!title_.empty() && title_ != title) {
            old = title_;
        }
        ledger_->RecordTitleChanged(title, old);
        return true;  // 落账失败由 ledger 记 I/O 错误(/doctor trajectory 可查)
    }
    return false;  // 没账可落(P0-6:旧 store 路已删)
}

SessionTitleAccount::LocalResult SessionTitleAccount::BeginLocalTitle(const std::string& first_query) {
    if (auto_attempted_ || !title_.empty()) {
        return LocalResult::NoNeed;
    }
    if (!LedgerActive()) {
        return LocalResult::NoNeed;  // 没账可落就没什么好起名的,/title 的人工路径照旧
    }
    auto_attempted_ = true;  // 一场只试一次,失败安静降级
    const std::string local = lubancode::app::LocalSessionTitle(first_query);
    if (local.empty()) {
        return LocalResult::NoUsableText;  // 首问没剩可看的字:标题留空,/sessions 用首句
    }
    return AdoptLocalTitle(local, /*quiet_on_failure=*/false);
}

SessionTitleAccount::LocalResult SessionTitleAccount::BackfillOnResume(const std::string& first_user_text) {
    generation_++;
    refiner_.RequestCancel();  // 上一场迟到的精炼结果不许落进新场子的存档
    auto_attempted_ = true;    // 恢复的场子不走"首问自动起名"路
    if (!title_.empty() || !LedgerActive()) {
        return LocalResult::NoNeed;
    }
    const std::string local = lubancode::app::LocalSessionTitle(first_user_text);
    if (local.empty()) {
        return LocalResult::NoUsableText;  // 老档没有可看的正文:标题留空,/sessions 用首句
    }
    // 老档补名失败安静退(quiet):不像首问路那样报一行,不拦人。
    return AdoptLocalTitle(local, /*quiet_on_failure=*/true);
}

SessionTitleAccount::AdoptResult SessionTitleAccount::AdoptRefined(
    const SessionTitleRefiner::Outcome& outcome) {
    if (!outcome.ok || outcome.title.empty()) {
        return AdoptResult::Ignored;  // 失败保留本地标题,不重试,不回落 normal
    }
    if (outcome.generation != generation_) {
        return AdoptResult::Ignored;  // 人工 /title、/clear 或 resume 抢先:迟到的自动结果丢弃
    }
    if (!LedgerActive()) {
        return AdoptResult::Ignored;  // 场子没了:标题无处落,不追着写
    }
    title_ = outcome.title;
    if (!AppendTitleEvent(title_)) {
        // 落不了盘就不占内存标题(老规矩),/sessions 仍用首句摘要。
        title_.clear();
        return AdoptResult::WriteFailed;
    }
    return AdoptResult::Adopted;
}

void SessionTitleAccount::ResetForNewSession() {
    // /clear 开新场:翻代、取消在飞精炼(迟到的落地即弃),下一问重走
    // 本地起名 + 精炼。
    generation_++;
    refiner_.RequestCancel();
    auto_attempted_ = false;
}

SessionTitleAccount::LocalResult SessionTitleAccount::AdoptLocalTitle(const std::string& local,
                                                                      bool quiet_on_failure) {
    title_ = local;
    if (!AppendTitleEvent(title_)) {
        // 落不了盘就不占内存标题(老规矩),/sessions 仍用首句摘要。
        title_.clear();
        return quiet_on_failure ? LocalResult::NoNeed : LocalResult::WriteFailed;
    }
    return LocalResult::Set;
}

}  // namespace lubancode::app
