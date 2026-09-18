// hook 包校验与试跑(LuaHook 单 P1-D,§8.2)。`lubancode hook validate`
// 与 `lubancode hook test` 共用的引擎;纯库无 CLI 依赖,测试直调。
//
// 校验分四档报告,不互相冒充:
//   1. 静态过  —— 清单 schema、entry 越界、Lua 语法与 handler 对账、
//                 依赖计划(发布裁决)、能力申请词表。脚本可编译不等于
//                 行为已验证。
//   2. fake 过 —— 真实 Lua runtime 跑 fixtures/ 下的用例;宿主 HTTP/
//                 文件/工具默认 fake adapter(fixture 里编排 canned 响应),
//                 零网络零业务文件。断言候选、效果、next 次数、结局与错码。
//   3. 真实集成过 —— 不在本命令里跑:须把包装进会话沿既有授权与真实宿主
//                 执行口观察(validate/test 不替信任流程开门,§8.2 第 4 条)。
//   4. 未验    —— 报告明列(真实服务、平台边界等)。
//
// fixture 形状(fixtures/*.json,一枚文件一枚用例):
//   {
//     "name": "cleans-whitespace",          // 缺省取文件名去后缀
//     "hookPoint": "PreUser",               // 缺省取清单第一条的挂点
//     "trigger": {                          // 必填;input 之外可带
//       "input": {"prompt": "  hello  "},   //   origin/purpose/deliveryMode/
//       "origin": "human", "purpose": "interactive", "deliveryMode": "steer"
//     },                                    //   turnId/stepId/stage(PreRequest 段)
//     "cancel": false,                      // 预置取消旗(注入取消用)
//     "http": [ {"status":200, "body":"…", "json":{} } ],  // fake HTTP 编排
//     "tools": [ {"name":"kb.search", "content":"…", "isError":false,
//                 "outcome":"timed_out", "schema":{}, "description":"…"} ],
//                                          // fake 工具;outcome 给了按终态分型
//                                          // 回错(unknown/cancelled 分支编排)
//     "expect": {                           // 断言(键都可选,给了才比)
//       "kind": "completed|denied|failed",
//       "prompt": "清洗后",                 // 采用后工作版本的 prompt 简写
//       "adoptedInput": {"prompt": "…"},    // 采用后工作版本全量
//       "value": {"…": …},                  // 链尾/短路值(子集匹配)
//       "denyCode": "input_rejected",
//       "errorCode": "hook.lua.runtime_error",
//       "contextAppends": ["…"],            // 已采用 context.append(计划序,全量)
//       "contextAppendsContain": ["…"],     // 同上的子串档(每段至少命中一条;executionId 动态时用)
//       "records": {"PreUser/prompt.normalize": {"outcome":"completed",
//                                               "nextCalls":1}},
//       "terminalRuns": 1                    // 链尾执行次数(短路 0)
//     }
//   }
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::runtime {

// 校验结论。exit_code 口径与 CLI 一致:0 全过 / 1 有 fail / 2 包读不到
// (不是 hook 包,或连 hook.json 都拿不出)。
struct HookCheckReport {
    struct Check {
        std::string item;   // 稳定检查项名(manifest/lua/plan/capabilities…)
        bool pass = false;
        std::string detail; // 过了给摘要,挂了给可定位的原因
    };
    struct Fixture {
        std::string name;
        bool ran = false;    // fake 试跑真跑了(静态挂了就不跑)
        bool pass = false;
        std::string detail;
    };

    std::string package_dir;
    std::vector<Check> checks;        // 静态检查(序固定)
    nlohmann::json plan;              // 解析后的实际计划(静态过才有)
    std::vector<Fixture> fixtures;    // fake 试跑(fake 档)
    std::vector<std::string> unverified;  // 未验项明列(第 4 档)

    bool static_pass = false;         // 静态档
    bool fixtures_present() const { return !fixtures.empty(); }
    bool fixtures_pass() const;       // 有用例且全过(零用例 = 没跑,不算过)

    int exit_code() const;
    nlohmann::json ToJson() const;
};

struct HookCheckOptions {
    bool run_fixtures = true;    // validate 缺省只跑静态;test 两档都跑
    // fake 文件写的落盘根(缺省每次调用造进程内临时目录,跑完即删;
    // 测试要保现场时显式给)。
    std::optional<std::filesystem::path> fs_scratch_root;
};

// 校验一枚 hook 包(package_dir 含 hook.json)。绝不抛;目录读不到按
// 第 2 档失败收口,不炸。
HookCheckReport RunHookPackageCheck(const std::filesystem::path& package_dir, HookCheckOptions options = {});

}  // namespace lubancode::runtime
