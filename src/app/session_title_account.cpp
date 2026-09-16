// SessionTitleAccount 的实现(骨架拆解反弹·问题 2):判断逻辑自
// TerminalSessionController 的四个标题方法逐字搬来,行为一字未改;打印、
// 模型路由、peer 同步留 controller。
#include "app/session_title_account.hpp"

#include <string>
#include <string_view>
#include <utility>

#include "app/session_title.hpp"

namespace lubancode::app {

// T11-A:生成身份——一代精炼一枚,起飞与落地同号(代数即会话内单调号,
// 号池不与 writer 主号撞名)。manual/local 来源不铸此号。
std::string SessionTitleAccount::TitleGenerationIdOf(std::uint64_t generation) {
    return "titlegen-" + std::to_string(generation);
}

SessionTitleAccount::SessionTitleAccount(std::string& title,
                                         lubancode::runtime::TrajectorySessionLedger* ledger)
    : title_(title), ledger_(ledger) {}

// P0-2:账本在,标题真账就是 control.title.changed(v3 场是
// session.title.applied);"档子活没活"看有没有能落账的写者——v2 看
// main recorder,v3 看 v3_main_writer(beta.1 反弹二:此前只认 v2 main,
// v3 场被拦在起名门外,首问自动起名与 /title 全不落账,标题列恒空)。
bool SessionTitleAccount::LedgerActive() const {
    return ledger_ != nullptr &&
           (ledger_->main() != nullptr || ledger_->v3_main_writer() != nullptr);
}

bool SessionTitleAccount::AppendTitleEvent(const std::string& title, std::string_view source,
                                            const std::string* title_generation_id) {
    if (ledger_ != nullptr) {
        if (!LedgerActive()) {
            return false;
        }
        std::string old;
        if (!title_.empty() && title_ != title) {
            old = title_;
        }
        // T11-A:来源如实分流——manual 走 RecordTitleChanged(/title 命令账);
        // local/generated 走 v3 分路(v2 场由账本兜底同归 control.title.
        // changed,老行为不变)。generated 带真 titleGenerationId,不伪造。
        if (source == "generated" && title_generation_id != nullptr) {
            ledger_->RecordGeneratedTitleApplied(*title_generation_id, title, old);
        } else if (source == "local") {
            ledger_->RecordLocalTitleApplied(title, old);
        } else {
            ledger_->RecordTitleChanged(title, old);
        }
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
    // T11-A:提取事实照记(title.extracted,带起飞时的生成身份)——迟到
    // 的生成也不例外,模型确实回了这句话;是否采用是另一枚事实。
    if (ledger_ != nullptr) {
        ledger_->RecordTitleExtracted(TitleGenerationIdOf(outcome.generation), outcome.title);
    }
    if (outcome.generation != generation_) {
        return AdoptResult::Ignored;  // 人工 /title、/clear 或 resume 抢先:迟到的自动结果不采用
    }
    if (!LedgerActive()) {
        return AdoptResult::Ignored;  // 场子没了:标题无处落,不追着写
    }
    const std::string generation_id = TitleGenerationIdOf(outcome.generation);
    title_ = outcome.title;
    if (!AppendTitleEvent(title_, "generated", &generation_id)) {
        // 落不了盘就不占内存标题(老规矩),/sessions 仍用首句摘要。
        title_.clear();
        return AdoptResult::WriteFailed;
    }
    return AdoptResult::Adopted;
}

void SessionTitleAccount::NoteTitleGenerationStarted(const std::string& model,
                                                      const std::string& provider) {
    // v3-only 事实(title.requested);v2 场账本侧 no-op。落账失败只记
    // I/O 错误——起飞是既成事实,不因记账失败取消采样。
    if (ledger_ != nullptr) {
        ledger_->RecordTitleRequested(TitleGenerationIdOf(generation_), model, provider);
    }
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
    if (!AppendTitleEvent(title_, "local", nullptr)) {
        // 落不了盘就不占内存标题(老规矩),/sessions 仍用首句摘要。
        title_.clear();
        return quiet_on_failure ? LocalResult::NoNeed : LocalResult::WriteFailed;
    }
    return LocalResult::Set;
}

}  // namespace lubancode::app
