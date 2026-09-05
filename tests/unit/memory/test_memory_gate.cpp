// 记忆写入调度单 P1:零成本门控的册——§7.1 必跳层逐案正反例、§7.3
// 中文最短正文门槛、MeaningfulTextStats 补全三项、§7.2 耐久信号
//(shadow 首折)与开关。全部纯函数离线判定,零网络零真模型。

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
#include <stdlib.h>
#endif

#include "app/memory_extract.hpp"

using namespace lubancode;

namespace {

// §7.1 案七(协议壳空)的夹具:只含 Thinking/Image 的增量,转写去壳为空。
api::Message ThinkingOnly() {
    api::Message message;
    message.role = api::Role::Assistant;
    message.content.push_back(api::ThinkingBlock{"内心独白不进转写"});
    return message;
}

app::MeaningfulTextStats Stats(const std::string& text) {
    return app::ComputeMeaningfulTextStats(text);
}

// 被拦时给 reason,过门给空。
std::string BlockedReason(const std::string& text, bool has_tool_evidence) {
    const auto blocked = app::EvaluateMustSkipTextGate(Stats(text), has_tool_evidence);
    if (!blocked.has_value()) return std::string();
    return app::ExtractionSkipReasonName(*blocked);
}

// 环境变量临时改写:出了作用域还原。
class EnvGuard final {
public:
    EnvGuard(const char* name, const char* value) : name_(name) {
        const char* old = std::getenv(name);
        if (old != nullptr) old_ = old;  // std::string 不吃空指针,先判再赋
        had_value_ = old != nullptr;
#ifdef _WIN32
        _putenv_s(name, value);
#else
        setenv(name, value, /*replace=*/1);
#endif
    }
    ~EnvGuard() {
#ifdef _WIN32
        if (had_value_) {
            _putenv_s(name_, old_.c_str());
        } else {
            _putenv_s(name_, "");
        }
#else
        if (had_value_) {
            setenv(name_, old_.c_str(), /*replace=*/1);
        } else {
            unsetenv(name_);
        }
#endif
    }

private:
    const char* name_;
    std::string old_;
    bool had_value_ = false;
};

}  // namespace

// ---------------------------------------------------------------------------
// §7.3 最短正文门:中文不能照抄 split(' ')>=3 的英文口径。
// ---------------------------------------------------------------------------
TEST_CASE("PassesMinimumTextGate: 三臂门槛逐条") {
    // CJK 臂:8 字过线,7 字不过。
    CHECK(app::PassesMinimumTextGate(Stats("这个崩溃怎么修的")));      // 8 CJK
    CHECK_FALSE(app::PassesMinimumTextGate(Stats("看看这个怎么修")));  // 7 CJK

    // 拉丁词臂:3 词过线,2 词不过。
    CHECK(app::PassesMinimumTextGate(Stats("run the tests")));
    CHECK_FALSE(app::PassesMinimumTextGate(Stats("ok boss")));

    // 代码记号臂:两枚代码记号 + 伴随自然语言,拉丁词不足 3 也过。
    CHECK(app::PassesMinimumTextGate(Stats("./x ./y")));
    // 只有一枚代码记号:不过。
    CHECK_FALSE(app::PassesMinimumTextGate(Stats("看下 x.sh")));
    // 代码记号齐了但没有自然语言(纯符号堆):不过。
    CHECK_FALSE(app::PassesMinimumTextGate(Stats("`.` `..`")));

    // 空白/标点:三臂全空,不过(§7.1 案三案四的兜底)。
    CHECK_FALSE(app::PassesMinimumTextGate(Stats("")));
    CHECK_FALSE(app::PassesMinimumTextGate(Stats("   \n\t  ")));
    CHECK_FALSE(app::PassesMinimumTextGate(Stats("，。 !?")));
}

// ---------------------------------------------------------------------------
// §7.1 必跳层:七种情况逐一,正例(拦 + 稳定 reason)配反例(放行)。
// ---------------------------------------------------------------------------
TEST_CASE("必跳层案一(write 关/extract 关):现行配置口径落 disabled") {
    // 案一的判定在 ExtractTurnMemory 现场(generate_enabled),枚举名
    // P0 已钉死;这里钉它不在文本门里产生——文本门不替案一发言。
    CHECK(Stats("/help").only_slash_command);
    CHECK(std::string(app::ExtractionSkipReasonName(app::ExtractionSkipReason::Disabled)) ==
          "disabled");
    // ExtractModeOff 是 P2 拆轴后的名,现行不产。
    CHECK(std::string(app::ExtractionSkipReasonName(app::ExtractionSkipReason::ExtractModeOff)) ==
          "extract_mode_off");
}

TEST_CASE("必跳层案二(同轮已变更):判定依据是回执账,不在文本门") {
    // 案二的判定在 MemoryTurnLedger::turn_mutated(见
    // test_memory_dispatch_ledger.cpp);文本门只保证不吞它的 reason。
    CHECK(std::string(app::ExtractionSkipReasonName(app::ExtractionSkipReason::AlreadyMutated)) ==
          "already_mutated");
}

TEST_CASE("必跳层案三(没有新增用户正文)与案四(空白/标点/UI 合成):short_text") {
    CHECK(BlockedReason("", false) == "short_text");
    CHECK(BlockedReason("   ", false) == "short_text");
    CHECK(BlockedReason("，。!?", false) == "short_text");
    // UI 合成一类短标记(查看戳、附件戳):够不成门槛。
    CHECK(BlockedReason("[已查看]", false) == "short_text");
    CHECK(BlockedReason("<image attached>", false) == "short_text");
    // 反例:真正文过线。
    CHECK(BlockedReason("帮我看看这个崩溃是怎么发生的", false).empty());
}

TEST_CASE("必跳层案五(只有宿主命令):slash_command_only") {
    CHECK(BlockedReason("/help", false) == "slash_command_only");
    CHECK(BlockedReason("/memory status", false) == "slash_command_only");
    CHECK(BlockedReason("  /exit  ", false) == "slash_command_only");
    // 反例:正文里引用命令,不是纯命令。
    CHECK(BlockedReason("看看 /help 都列了什么命令，挑两条常用的讲讲", false).empty());
    // 反例:以斜杠开头的路径正文不误伤——首字符不是 '/'。
    CHECK(BlockedReason("把 src/app 下的入口都梳理一遍，写进文档里", false).empty());
}

TEST_CASE("必跳层案六(短确认/短否定/继续指令且无工具证据):acknowledgement_only") {
    // 短确认。
    CHECK(BlockedReason("好", false) == "acknowledgement_only");
    CHECK(BlockedReason("嗯嗯，继续吧", false) == "acknowledgement_only");
    CHECK(BlockedReason("ok, go on", false) == "acknowledgement_only");
    // 短否定。
    CHECK(BlockedReason("不用了，算了", false) == "acknowledgement_only");
    CHECK(BlockedReason("nope", false) == "acknowledgement_only");
    // 继续指令。
    CHECK(BlockedReason("继续", false) == "acknowledgement_only");
    CHECK(BlockedReason("下一步", false) == "acknowledgement_only");
    // 同样的话带上了新工具证据:案六不拦(§7.1 的"且没有新工具证据"),
    // 落到门槛上按 short_text 拦——确认短语天然过不了最短正文门。
    CHECK(BlockedReason("好", true) == "short_text");
    CHECK(BlockedReason("继续", true) == "short_text");
    // 反例:确认后头跟着正事,不是纯确认。
    CHECK(BlockedReason("好的，把 README 里安装那段补上 pnpm 的说明", false).empty());
    CHECK(BlockedReason("继续，把刚才那个崩溃的根因写进文档", false).empty());
    CHECK(BlockedReason("不用 pnpm，这个项目固定用 npm ci，lockfile 是 npm 生成的", false)
              .empty());
}

TEST_CASE("必跳层案七(新增 transcript 去协议壳后为空):empty_transcript 在调用点") {
    // 只含 Thinking/Image 的增量:转写为空,ExtractTurnMemory 按既有
    // EmptyTranscript 门拦(案七),这里钉转写确实为空。
    std::vector<api::Message> shell_only;
    shell_only.push_back(ThinkingOnly());
    CHECK(app::BuildTurnTranscript(shell_only, 24 * 1024).empty());
    CHECK(std::string(app::ExtractionSkipReasonName(app::ExtractionSkipReason::EmptyTranscript)) ==
          "empty_transcript");
}

// ---------------------------------------------------------------------------
// MeaningfulTextStats 补全三项(§3.2):代码记号、纯确认、纯命令。
// ---------------------------------------------------------------------------
TEST_CASE("ComputeMeaningfulTextStats: code_token_count 的词法口径") {
    // 反引号段:一段一记。
    CHECK(Stats("跑 `cmake --build` 跑通了").code_token_count == 1);
    CHECK(Stats("`a` `b`").code_token_count == 2);
    // 反引号段内容为空:不计。
    CHECK(Stats("`` ``").code_token_count == 0);
    // 裸词带代码记号(_ . / = : < > # @ $):计。
    CHECK(Stats("用 build.sh 跑构建，配置在 CMakeLists.txt 里").code_token_count == 2);
    CHECK(Stats("std::string 换 std::vector").code_token_count == 2);
    CHECK(Stats("./x ./y").code_token_count == 2);
    // 纯字母词、连字符词、纯数字:不计。
    CHECK(Stats("pnpm install").code_token_count == 0);
    CHECK(Stats("well-known solution").code_token_count == 0);
    CHECK(Stats("123 456").code_token_count == 0);
    // 没配对的反引号:当裸字,不算段。
    CHECK(Stats("`x").code_token_count == 0);
}

TEST_CASE("ComputeMeaningfulTextStats: only_acknowledgement") {
    CHECK(Stats("好").only_acknowledgement);
    CHECK(Stats("嗯嗯，继续吧").only_acknowledgement);
    CHECK(Stats("好的呀").only_acknowledgement);  // 剥尾助词后整词命中
    CHECK(Stats("ok, go on").only_acknowledgement);
    CHECK(Stats("不用了。").only_acknowledgement);
    // 反例:确认后头跟着正事/多节里混了内容。
    CHECK_FALSE(Stats("好，那把这个也改了").only_acknowledgement);
    CHECK_FALSE(Stats("ok boss").only_acknowledgement);
    CHECK_FALSE(Stats("继续改这个 bug").only_acknowledgement);
    CHECK_FALSE(Stats("").only_acknowledgement);
    // 纯助词剥成空串:不算确认。
    CHECK_FALSE(Stats("吧呀").only_acknowledgement);
}

TEST_CASE("ComputeMeaningfulTextStats: only_slash_command") {
    CHECK(Stats("/help").only_slash_command);
    CHECK(Stats("  /memory status").only_slash_command);
    CHECK_FALSE(Stats("看看 /help").only_slash_command);
    CHECK_FALSE(Stats("").only_slash_command);
    CHECK_FALSE(Stats("普通正文").only_slash_command);
}

// ---------------------------------------------------------------------------
// §7.2 耐久信号(shadow 首折):宁可保守,正反例钉住冻结名单。
// ---------------------------------------------------------------------------
TEST_CASE("EvaluateDurableSignals: 逐案正反例") {
    const auto signals = [](const std::string& text, bool tools, bool mutated) {
        return app::EvaluateDurableSignals(text, Stats(text), tools, mutated);
    };
    const auto contains = [](const std::vector<std::string>& list, const char* name) {
        return std::find(list.begin(), list.end(), std::string(name)) != list.end();
    };

    // 案一:跨回合偏好/禁忌/纠错。
    CHECK(contains(signals("以后统一用 pnpm 装依赖，别再用 npm", false, false),
                   "preference_or_correction"));
    CHECK(contains(signals("From now on always use uv for installs", false, false),
                   "preference_or_correction"));
    // 反例:普通请求不带偏好。
    CHECK(signals("帮我看看这个文件哪里坏了", true, false).empty());

    // 案二:配置/构建合同变更,须有工具证据。
    CHECK(contains(signals("升级了 cmake 版本，构建脚本要跟着改", true, false),
                   "config_or_build_change"));
    CHECK_FALSE(contains(signals("升级了 cmake 版本，构建脚本要跟着改", false, false),
                         "config_or_build_change"));

    // 案三:测试/诊断稳定结论,须有工具证据。
    CHECK(contains(signals("测试全绿，这次回归没有翻车", true, false), "test_conclusion"));
    CHECK_FALSE(contains(signals("测试全绿，这次回归没有翻车", false, false), "test_conclusion"));

    // 案四:模块边界/命令入口/操作约束,须有工具证据。
    CHECK(contains(signals("把工具入口拆出独立模块，注册了新命令", true, false),
                   "module_boundary_or_entry"));
    CHECK_FALSE(contains(signals("把工具入口拆出独立模块，注册了新命令", false, false),
                         "module_boundary_or_entry"));

    // 案五:点名要记、主回合未存成;存成了(变更过)不记。
    CHECK(contains(signals("记住这个项目用 pnpm", false, false), "explicit_remember_unsaved"));
    CHECK_FALSE(contains(signals("记住这个项目用 pnpm", false, true), "explicit_remember_unsaved"));

    // 案六(compact 未审材料):P1 恒不命中,名字不在任何输出里。
    const auto none = signals("随便聊点什么", true, false);
    CHECK(none.empty());
}

// ---------------------------------------------------------------------------
// shadow 开关:环境变量启停,默认关。
// ---------------------------------------------------------------------------
TEST_CASE("MemoryGateShadowEnabled: 环境变量开关") {
    CHECK_FALSE(app::MemoryGateShadowEnabled());  // 默认关(前提:外场没设)
    {
        EnvGuard on("LUBANCODE_MEMORY_GATE_SHADOW", "1");
        CHECK(app::MemoryGateShadowEnabled());
    }
    CHECK_FALSE(app::MemoryGateShadowEnabled());
    {
        EnvGuard truthy("LUBANCODE_MEMORY_GATE_SHADOW", "true");
        CHECK(app::MemoryGateShadowEnabled());
    }
    {
        EnvGuard other("LUBANCODE_MEMORY_GATE_SHADOW", "yes");
        CHECK(app::MemoryGateShadowEnabled());
    }
    {
        EnvGuard off("LUBANCODE_MEMORY_GATE_SHADOW", "0");
        CHECK_FALSE(app::MemoryGateShadowEnabled());
    }
    {
        EnvGuard junk("LUBANCODE_MEMORY_GATE_SHADOW", "随便");
        CHECK_FALSE(app::MemoryGateShadowEnabled());
    }
}
