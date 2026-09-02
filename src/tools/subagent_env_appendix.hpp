// 派工任务书单 2.1(P1)/2.2(P2):本机环境附录与六件套套壳。
//
// 子代理进场没有父会话上下文,构建环境(preset 名、离线 _deps、ctest 规矩、
// --clean-first)全靠自己摸,烧 turn 烧 token——对照主控实战任务书,一次
// 跑成率天差地别(单子"现场 C")。这里把"探测一次的本机构建事实"折成一段
// 附录正文,由 AgentTool 在派工 prompt 尾部自动附上(注入落点在
// agent_tool.cpp 的 ExecuteDispatch,不重写用户正文);六件套任务书壳
// (template: full)也住这只模块,原文逐字节居中,壳只做引导。
//
// 探测只读文件系统与 CMakePresets.json,不跑 git(git 仓库根由装配层
// FindRepositoryRoot 先折好递进来)、不碰网;纯函数,无状态。
#pragma once

#include <filesystem>
#include <string>

namespace lubancode::tools {

// 启动时探测一次的本机构建环境事实(会话内缓存复用,不逐派工重探)。
struct SubagentEnvFacts {
    // 仓库根有 CMakeLists.txt 才算 CMake 工程;否则整段附录不注入(别的
    // 构建体系的仓库,硬塞 CMake 建议只是噪声)。
    bool is_cmake_project = false;
    // 选中的 configure preset 名(空 = 没声明或读不动)。
    std::string preset_name;
    // 多配置生成器的配置名(ctest -C 与 cmake --build --config 用;缺省 Release)。
    std::string build_config;
    // 构建目录(相对仓库根的正斜杠路径,便于塞进 prompt;探不出为空)。
    std::string build_dir_utf8;
    // 构建目录已在。
    bool build_tree_present = false;
    // 构建目录下 _deps 非空(离线 configure 的料齐了)。
    bool offline_deps_ready = false;
};

// 探测:读 CMakePresets.json(有 CMakeUserPresets.json 一并并入,名字
// 冲突以基础文件先到先得),preset 挑法——已有配置树(CMakeCache.txt 在)
// 的优先,其次名单里叫 release 的(仓库工作惯例,子代理验证构建用
// release 档),再退名单第一个;全没声明时扫 build/ 下第一个带
// CMakeCache.txt 的子目录。异常一律吞掉折"探测不出",不挡会话起。
SubagentEnvFacts DetectSubagentEnvFacts(const std::filesystem::path& repo_root);

// 事实 -> 附录正文。非 CMake 工程返回空串(调用方据此不注入)。首行带
// [宿主注入·本机环境附录] 来源标注,与用户正文不混淆;正文按语言查
// src/prompts/tools/<语言>/agent.md 的 env_appendix.* 节({0}/{1} 占位),
// 查表不到用调用方的中文兜底。
std::string ComposeSubagentEnvAppendix(const SubagentEnvFacts& facts);

// 六件套套壳(template: full):单子路径/范围红线/环境实情/纪律/完工标准/
// 回报格式。原文逐字节居中,壳只引导不强制——原文写清的不必重抄,缺的项
// 提示子代理补齐或问明,不脑补。文案查同表的 template.full 节,{0} 是原文。
std::string WrapPromptWithTaskTemplate(const std::string& user_prompt);

}  // namespace lubancode::tools
