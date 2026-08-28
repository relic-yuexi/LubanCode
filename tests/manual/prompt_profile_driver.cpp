// prompt_profile_driver:Prompt Profile 单(阶段 2)的黄金 dump 驱动,不进
// ctest,验收手动跑:
//   prompt_profile_driver default [用户目录] [项目目录]
//       未选 Profile 的 default 拼装全文(后两参可省;给了也只影响
//       "用户全局 default 覆盖"层——default 上下文没有项目层)
//   prompt_profile_driver persona
//       法替换 core 的对照全文
//   prompt_profile_driver profile <名字> <用户目录> [项目目录]
//       选中 Profile 的拼装全文
//   prompt_profile_driver ledger <名字> <用户目录> [项目目录]
//       整表来源账本(BuildPromptProfileLedger 的逐行输出)
// 用法:基线(改动前)与改后各跑一遍 default,重定向落盘,逐字节 diff——
// "未选 Profile 时黄金输出零 diff"的直接证据。测试册里的重构断言是数学
// 证据,这里是字节证据,两条互为印证。
#include <cstdio>
#include <cstring>
#include <string>

#include "agent/prompt_assembler.hpp"
#include "agent/prompts.hpp"

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "default";
    lubancode::agent::PromptOptions options;
    options.cwd = "D:/work";
    options.current_date = "2026-07-18";
    if (mode == "default") {
        if (argc > 2) {
            options.prompts_dir = argv[2];
        }
        if (argc > 3) {
            options.project_prompts_dir = argv[3];  // default 上下文:项目层不参与,给了也不读
        }
        std::fputs(lubancode::agent::AssembleSystemPrompt(options).c_str(), stdout);
        return 0;
    }
    if (mode == "persona") {
        options.persona = "你是测试人格。";
        std::fputs(lubancode::agent::AssembleSystemPrompt(options).c_str(), stdout);
        return 0;
    }
    if ((mode == "profile" || mode == "ledger") && argc > 3) {
        options.profile = argv[2];
        options.prompts_dir = argv[3];
        if (argc > 4) {
            options.project_prompts_dir = argv[4];
        }
        if (mode == "profile") {
            std::fputs(lubancode::agent::AssembleSystemPrompt(options).c_str(), stdout);
        } else {
            const lubancode::agent::PromptSourceLedger ledger =
                lubancode::agent::BuildPromptProfileLedger(options.profile, options.prompts_dir,
                                                           options.project_prompts_dir);
            for (const auto& entry : ledger.entries) {
                std::fputs(entry.FormatLine().c_str(), stdout);
                std::fputc('\n', stdout);
            }
        }
        return 0;
    }
    std::fprintf(stderr, "用法: %s default [用户目录] [项目目录] | persona | "
                         "profile|ledger <名字> <用户目录> [项目目录]\n",
                 argc > 0 ? argv[0] : "prompt_profile_driver");
    return 2;
}
