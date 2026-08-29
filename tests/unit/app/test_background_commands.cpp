// /background 二级参数的纯解析与底栏状态段折数(照 ParsePackageCommand
// 单测的路数钉住;handler 的台账读写另在 integration/process 那册真起进程)。

#include <doctest/doctest.h>

#include <string>

#include "app/commands/background_commands.hpp"
#include "tools/background_tasks.hpp"

namespace {

const char* ActionName(lubancode::app::BackgroundCommandAction action) {
    switch (action) {
        case lubancode::app::BackgroundCommandAction::Invalid: return "Invalid";
        case lubancode::app::BackgroundCommandAction::List: return "List";
        case lubancode::app::BackgroundCommandAction::Show: return "Show";
        case lubancode::app::BackgroundCommandAction::Logs: return "Logs";
        case lubancode::app::BackgroundCommandAction::Stop: return "Stop";
    }
    return "?";
}

lubancode::tools::BackgroundTaskInfo MakeTask(const char* id, lubancode::tools::BackgroundTaskStatus status) {
    lubancode::tools::BackgroundTaskInfo info;
    info.task_id = id;
    info.status = status;
    return info;
}

}  // namespace

TEST_CASE("background.parse:裸敲与 list") {
    CHECK(lubancode::app::ParseBackgroundCommand("").action == lubancode::app::BackgroundCommandAction::List);
    CHECK(lubancode::app::ParseBackgroundCommand("   ").action ==
          lubancode::app::BackgroundCommandAction::List);
    const auto list = lubancode::app::ParseBackgroundCommand("list");
    CHECK(list.action == lubancode::app::BackgroundCommandAction::List);
    // 大小写不敏感。
    CHECK(lubancode::app::ParseBackgroundCommand("  LIST ").action ==
          lubancode::app::BackgroundCommandAction::List);
    // list 不收参数。
    const auto bad = lubancode::app::ParseBackgroundCommand("list extra");
    CHECK(bad.action == lubancode::app::BackgroundCommandAction::Invalid);
    CHECK(bad.bad_word == "extra");
}

TEST_CASE("background.parse:show 带目标;缺目标 Invalid") {
    const auto show = lubancode::app::ParseBackgroundCommand("show 2");
    CHECK(show.action == lubancode::app::BackgroundCommandAction::Show);
    CHECK(show.target == "2");
    // 大小写不敏感;前后空白剥掉。
    const auto upper = lubancode::app::ParseBackgroundCommand("  SHOW  12 ");
    CHECK(upper.action == lubancode::app::BackgroundCommandAction::Show);
    CHECK(upper.target == "12");
    // 缺目标 Invalid,bad_word 记动词,提示补 id。
    const auto missing = lubancode::app::ParseBackgroundCommand("show");
    CHECK(missing.action == lubancode::app::BackgroundCommandAction::Invalid);
    CHECK(missing.bad_word == "show");
    // 多余的词不猜,Invalid。
    const auto extra = lubancode::app::ParseBackgroundCommand("show 2 3");
    CHECK(extra.action == lubancode::app::BackgroundCommandAction::Invalid);
    CHECK(extra.bad_word == "3");
}

TEST_CASE("background.parse:logs 的 id 与 --tail") {
    const auto plain = lubancode::app::ParseBackgroundCommand("logs 2");
    CHECK(plain.action == lubancode::app::BackgroundCommandAction::Logs);
    CHECK(plain.target == "2");
    CHECK(plain.tail_lines == 100);  // 缺省 100(单子定的,别跟工具的 50 混)

    // --tail N(空格分隔)。
    const auto spaced = lubancode::app::ParseBackgroundCommand("logs 2 --tail 30");
    CHECK(spaced.action == lubancode::app::BackgroundCommandAction::Logs);
    CHECK(spaced.target == "2");
    CHECK(spaced.tail_lines == 30);

    // --tail=N(等号连写)。
    const auto glued = lubancode::app::ParseBackgroundCommand("logs 3 --tail=7");
    CHECK(glued.action == lubancode::app::BackgroundCommandAction::Logs);
    CHECK(glued.target == "3");
    CHECK(glued.tail_lines == 7);

    // 大小写与多空白。
    const auto upper = lubancode::app::ParseBackgroundCommand("LOGS 5  --tail  9");
    CHECK(upper.action == lubancode::app::BackgroundCommandAction::Logs);
    CHECK(upper.tail_lines == 9);

    // 0 与负数照收(与 background_output 同语义:<=0 查全文),解析层不裁。
    CHECK(lubancode::app::ParseBackgroundCommand("logs 2 --tail 0").tail_lines == 0);
    CHECK(lubancode::app::ParseBackgroundCommand("logs 2 --tail -5").tail_lines == 0);

    // 缺目标 / --tail 缺数 / 非整数,各记各的坏词。
    const auto missing_id = lubancode::app::ParseBackgroundCommand("logs");
    CHECK(missing_id.action == lubancode::app::BackgroundCommandAction::Invalid);
    CHECK(missing_id.bad_word == "logs");
    const auto flag_first = lubancode::app::ParseBackgroundCommand("logs --tail 5");
    CHECK(flag_first.action == lubancode::app::BackgroundCommandAction::Invalid);
    const auto tail_no_num = lubancode::app::ParseBackgroundCommand("logs 2 --tail");
    CHECK(tail_no_num.action == lubancode::app::BackgroundCommandAction::Invalid);
    CHECK(tail_no_num.bad_word == "--tail");
    const auto tail_bad_num = lubancode::app::ParseBackgroundCommand("logs 2 --tail abc");
    CHECK(tail_bad_num.action == lubancode::app::BackgroundCommandAction::Invalid);
    const auto tail_bad_glue = lubancode::app::ParseBackgroundCommand("logs 2 --tail=xx");
    CHECK(tail_bad_glue.action == lubancode::app::BackgroundCommandAction::Invalid);
    CHECK(tail_bad_glue.bad_word == "--tail=xx");
    // 认不得的旗子不吞。
    const auto unknown_flag = lubancode::app::ParseBackgroundCommand("logs 2 --follow");
    CHECK(unknown_flag.action == lubancode::app::BackgroundCommandAction::Invalid);
    CHECK(unknown_flag.bad_word == "--follow");
}

TEST_CASE("background.parse:stop 的单停与全停") {
    const auto single = lubancode::app::ParseBackgroundCommand("stop 2");
    CHECK(single.action == lubancode::app::BackgroundCommandAction::Stop);
    CHECK(single.target == "2");
    CHECK_FALSE(single.stop_all);

    const auto all = lubancode::app::ParseBackgroundCommand("stop all");
    CHECK(all.action == lubancode::app::BackgroundCommandAction::Stop);
    CHECK(all.stop_all);
    CHECK(all.target.empty());
    // 大小写不敏感。
    CHECK(lubancode::app::ParseBackgroundCommand("STOP ALL").stop_all);

    // 缺目标 Invalid;all 后跟东西 Invalid。
    const auto missing = lubancode::app::ParseBackgroundCommand("stop");
    CHECK(missing.action == lubancode::app::BackgroundCommandAction::Invalid);
    CHECK(missing.bad_word == "stop");
    const auto all_extra = lubancode::app::ParseBackgroundCommand("stop all now");
    CHECK(all_extra.action == lubancode::app::BackgroundCommandAction::Invalid);
    CHECK(all_extra.bad_word == "now");
    const auto single_extra = lubancode::app::ParseBackgroundCommand("stop 1 2");
    CHECK(single_extra.action == lubancode::app::BackgroundCommandAction::Invalid);
}

TEST_CASE("background.parse:认不得的动词一律 Invalid") {
    for (const char* bad : {"start", "tail 2", "kill 2", "follow 2", "logs2"}) {
        INFO(bad);
        const auto parsed = lubancode::app::ParseBackgroundCommand(bad);
        CHECK(parsed.action == lubancode::app::BackgroundCommandAction::Invalid);
        CHECK_FALSE(parsed.bad_word.empty());
    }
    CHECK(lubancode::app::ParseBackgroundCommand("xyz").bad_word == "xyz");
}

TEST_CASE("background.status:折数走「运行/完成」两 bucket,空账收起") {
    using lubancode::tools::BackgroundTaskStatus;
    CHECK(lubancode::app::BuildBackgroundStatusSegment({}).empty());  // 没任务:段收起

    // 两只在跑(停止中算"还在跑"这边),一只收场。
    const std::vector<lubancode::tools::BackgroundTaskInfo> mixed = {
        MakeTask("1", BackgroundTaskStatus::Running),
        MakeTask("2", BackgroundTaskStatus::Stopping),
        MakeTask("3", BackgroundTaskStatus::Completed),
    };
    CHECK(lubancode::app::BuildBackgroundStatusSegment(mixed) == "后台 2 运行 / 1 完成");

    // 只在跑:不带"完成"尾巴。
    const std::vector<lubancode::tools::BackgroundTaskInfo> running_only = {
        MakeTask("1", BackgroundTaskStatus::Running),
    };
    CHECK(lubancode::app::BuildBackgroundStatusSegment(running_only) == "后台 1 运行");

    // 全收场:完成/失败/已停止/停止失败都算"完成"。
    const std::vector<lubancode::tools::BackgroundTaskInfo> done = {
        MakeTask("1", BackgroundTaskStatus::Completed),
        MakeTask("2", BackgroundTaskStatus::Failed),
        MakeTask("3", BackgroundTaskStatus::Stopped),
        MakeTask("4", BackgroundTaskStatus::StopFailed),
    };
    CHECK(lubancode::app::BuildBackgroundStatusSegment(done) == "后台 4 完成");
}
