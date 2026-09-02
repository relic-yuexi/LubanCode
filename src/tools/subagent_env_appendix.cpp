// 本模块的合同见 subagent_env_appendix.hpp 顶注。探测纯读文件系统;
// 成文查 prompts 表(src/prompts/tools/<语言>/agent.md),兜底是这里的
// 中文原文——表改文案不用重编语义,键漏了也不空。
#include "tools/subagent_env_appendix.hpp"

#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "tools/tool_text.hpp"

namespace lubancode::tools {

namespace {

// {n} 占位替换(与 cli/i18n 的 trf 同一套 {0} 风格;替换后不重扫塞进去的
// 值,用户正文里带 {0} 也不会被二次展开)。
void ReplacePlaceholder(std::string& text, const std::string& placeholder, const std::string& value) {
    std::size_t at = text.find(placeholder);
    while (at != std::string::npos) {
        text.replace(at, placeholder.size(), value);
        at = text.find(placeholder, at + value.size());
    }
}

std::string FormatText(std::string text, const std::vector<std::pair<std::string, std::string>>& args) {
    for (const auto& [placeholder, value] : args) {
        ReplacePlaceholder(text, placeholder, value);
    }
    return text;
}

// 一个 preset 候选:${sourceDir} 已替换成仓库根,configured = 构建目录里
// 有 CMakeCache.txt(真配置过的树,不是空目录)。
struct PresetCandidate {
    std::string name;
    std::filesystem::path build_dir;
    std::string config;
    bool configured = false;
};

// 把 CMakePresets.json 里 configurePresets 的 cacheVariables 配置名折出来
// (CMAKE_CONFIGURATION_TYPES 可能是 "Debug;Release;..." 多值,取第一枚;
// 单配置生成器没有这键,交给 buildPresets 或缺省)。
std::string ConfigOfPreset(const nlohmann::json& preset) {
    if (preset.contains("cacheVariables") && preset["cacheVariables"].is_object()) {
        const auto& vars = preset["cacheVariables"];
        if (vars.contains("CMAKE_CONFIGURATION_TYPES") && vars["CMAKE_CONFIGURATION_TYPES"].is_string()) {
            const std::string types = vars["CMAKE_CONFIGURATION_TYPES"].get<std::string>();
            const std::size_t sep = types.find(';');
            std::string first = sep == std::string::npos ? types : types.substr(0, sep);
            if (!first.empty()) {
                return first;
            }
        }
    }
    return std::string();
}

// 一个 presets 文件折出候选清单(顺带吃同文件的 buildPresets,把配置名
// 对上);读不动/坏 JSON 返回空,不抛。
std::vector<PresetCandidate> ParsePresetFile(const std::filesystem::path& file,
                                             const std::filesystem::path& repo_root) {
    std::vector<PresetCandidate> out;
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) {
        return out;
    }
    std::ifstream input(file, std::ios::binary);
    if (!input) {
        return out;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const nlohmann::json doc = nlohmann::json::parse(buffer.str(), nullptr, false);
    if (doc.is_discarded() || !doc.contains("configurePresets") || !doc["configurePresets"].is_array()) {
        return out;
    }
    const std::string source_dir = repo_root.generic_string();
    for (const auto& preset : doc["configurePresets"]) {
        if (!preset.is_object() || !preset.contains("name") || !preset["name"].is_string()) {
            continue;
        }
        PresetCandidate candidate;
        candidate.name = preset["name"].get<std::string>();
        // ${sourceDir} 是 presets 里最常见的占位;只做字面替换,${presetName}
        // 等罕见占位当相对路径处理,不展开。
        std::string binary_dir = preset.value("binaryDir", std::string());
        if (binary_dir.empty()) {
            candidate.build_dir = repo_root / "build" / candidate.name;
        } else {
            std::size_t at = binary_dir.find("${sourceDir}");
            while (at != std::string::npos) {
                binary_dir.replace(at, std::string("${sourceDir}").size(), source_dir);
                at = binary_dir.find("${sourceDir}", at + source_dir.size());
            }
            candidate.build_dir = std::filesystem::path(binary_dir).lexically_normal();
        }
        candidate.config = ConfigOfPreset(preset);
        std::error_code cache_ec;
        candidate.configured = std::filesystem::exists(candidate.build_dir / "CMakeCache.txt", cache_ec);
        out.push_back(std::move(candidate));
    }
    // buildPresets 的 configuration 补漏:configure 候选没从 cacheVariables
    // 折出配置名时,按 configurePreset 名对上取。
    if (doc.contains("buildPresets") && doc["buildPresets"].is_array()) {
        for (const auto& build : doc["buildPresets"]) {
            if (!build.is_object() || !build.contains("configurePreset") || !build["configurePreset"].is_string() ||
                !build.contains("configuration") || !build["configuration"].is_string()) {
                continue;
            }
            const std::string target = build["configurePreset"].get<std::string>();
            for (auto& candidate : out) {
                if (candidate.name == target && candidate.config.empty()) {
                    candidate.config = build["configuration"].get<std::string>();
                }
            }
        }
    }
    return out;
}

// 探测本体(可能抛 filesystem 异常的路径都罩在这,外层吞)。
SubagentEnvFacts DetectIn(const std::filesystem::path& repo_root) {
    SubagentEnvFacts facts;
    std::error_code probe_ec;
    if (repo_root.empty() || !std::filesystem::exists(repo_root / "CMakeLists.txt", probe_ec)) {
        return facts;  // 不是 CMake 工程:整段附录不注入
    }
    facts.is_cmake_project = true;

    std::vector<PresetCandidate> candidates = ParsePresetFile(repo_root / "CMakePresets.json", repo_root);
    for (PresetCandidate& extra : ParsePresetFile(repo_root / "CMakeUserPresets.json", repo_root)) {
        bool duplicate = false;
        for (const auto& existing : candidates) {
            if (existing.name == extra.name) {
                duplicate = true;  // 基础文件先到先得(user 文件不许遮基础名)
                break;
            }
        }
        if (!duplicate) {
            candidates.push_back(std::move(extra));
        }
    }

    const PresetCandidate* chosen = nullptr;
    for (const auto& candidate : candidates) {
        if (candidate.configured) {
            chosen = &candidate;  // 已有配置树的 preset 最可信
            break;
        }
    }
    if (chosen == nullptr) {
        for (const auto& candidate : candidates) {
            if (candidate.name == "release") {
                chosen = &candidate;  // 仓库工作惯例:验证构建走 release 档
                break;
            }
        }
    }
    if (chosen == nullptr && !candidates.empty()) {
        chosen = &candidates.front();
    }

    std::filesystem::path build_dir;
    if (chosen != nullptr) {
        facts.preset_name = chosen->name;
        facts.build_config = chosen->config.empty() ? std::string("Release") : chosen->config;
        build_dir = chosen->build_dir;
    } else {
        // 没声明 preset 的 CMake 工程:扫 build/ 下第一个带 CMakeCache.txt
        // 的子目录,好歹把"build 树在不在、_deps 齐不齐"说对。
        facts.build_config = "Release";
        const std::filesystem::path build_root = repo_root / "build";
        std::error_code scan_ec;
        if (std::filesystem::exists(build_root, scan_ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(build_root)) {
                if (!entry.is_directory()) {
                    continue;
                }
                std::error_code cache_ec;
                if (std::filesystem::exists(entry.path() / "CMakeCache.txt", cache_ec)) {
                    build_dir = entry.path();
                    break;
                }
            }
        }
    }

    if (build_dir.empty()) {
        return facts;
    }
    // 显示口径:优先相对仓库根的正斜杠路径;目录在仓库外(自定义
    // binaryDir 指到别处)就原样绝对路径,不说谎。
    std::error_code rel_ec;
    const std::filesystem::path relative = std::filesystem::relative(build_dir, repo_root, rel_ec);
    if (!rel_ec && !relative.empty() && !relative.is_absolute()) {
        facts.build_dir_utf8 = relative.generic_string();
    } else {
        facts.build_dir_utf8 = build_dir.generic_string();
    }
    std::error_code dir_ec;
    facts.build_tree_present = std::filesystem::exists(build_dir, dir_ec);
    {
        std::error_code deps_ec;
        const std::filesystem::path deps = build_dir / "_deps";
        if (std::filesystem::is_directory(deps, deps_ec)) {
            std::error_code empty_ec;
            facts.offline_deps_ready = !std::filesystem::is_empty(deps, empty_ec);
        }
    }
    return facts;
}

}  // namespace

SubagentEnvFacts DetectSubagentEnvFacts(const std::filesystem::path& repo_root) {
    try {
        return DetectIn(repo_root);
    } catch (...) {
        return SubagentEnvFacts{};  // 探测不出就当没探到,不挡派工
    }
}

std::string ComposeSubagentEnvAppendix(const SubagentEnvFacts& facts) {
    if (!facts.is_cmake_project) {
        return std::string();
    }
    std::string out = ToolText(
        "agent", "env_appendix.header",
        "[宿主注入·本机环境附录] 以下是宿主在会话启动时探测的本机构建环境事实,与任务正文无关;构建与测试照此办理,不必自行摸索。");
    out += "\n- " +
           FormatText(ToolText("agent", "env_appendix.preset",
                               "仓库构建账:CMake preset {0};构建目录 {1}(相对仓库根);ctest 配置名 {2}。"),
                      {{"{0}", facts.preset_name.empty() ? std::string("(未声明)") : facts.preset_name},
                       {"{1}", facts.build_dir_utf8.empty() ? std::string("(未探得)") : facts.build_dir_utf8},
                       {"{2}", facts.build_config.empty() ? std::string("Release") : facts.build_config}});
    if (facts.offline_deps_ready) {
        out += "\n- " + FormatText(ToolText("agent", "env_appendix.offline_deps_ready",
                                            "离线省时路:本机 {0}/_deps 已备齐。换树构建时把它整目录拷到该树同名位置,"
                                            "configure 再加 -DFETCHCONTENT_FULLY_DISCONNECTED=ON(依赖全走本地,网络不通"
                                            "也能配);全量 FetchContent configure 约十分钟,能省则省。"),
                                   {{"{0}", facts.build_dir_utf8}});
    } else if (facts.build_tree_present) {
        out += "\n- " + FormatText(ToolText("agent", "env_appendix.offline_no_deps",
                                            "构建树已在,但 {0}/_deps 不齐:离线路走不通,依赖走 FetchContent 全量 "
                                            "configure(约十分钟)。"),
                                   {{"{0}", facts.build_dir_utf8}});
    } else {
        out += "\n- " + ToolText("agent", "env_appendix.offline_no_build",
                                 "本机还没起构建树:先 configure(依赖走 FetchContent,约十分钟);别处若有备齐的 "
                                 "_deps,整目录拷来并加 -DFETCHCONTENT_FULLY_DISCONNECTED=ON 可走离线。");
    }
#ifdef _WIN32
    out += "\n- " + FormatText(ToolText("agent", "env_appendix.ctest_windows",
                                        "ctest 规矩:必带 -C {0}(多配置生成器);ctest 前把 USERPROFILE 指到 "
                                        "Windows 路径的临时目录,别碰真用户主目录。"),
                               {{"{0}", facts.build_config.empty() ? std::string("Release") : facts.build_config}});
#else
    out += "\n- " + FormatText(ToolText("agent", "env_appendix.ctest_posix",
                                        "ctest 规矩:多配置生成器必带 -C {0}。"),
                               {{"{0}", facts.build_config.empty() ? std::string("Release") : facts.build_config}});
#endif
    out += "\n- " + ToolText("agent", "env_appendix.clean_first",
                             "动了头文件就带 --clean-first 重建,别吃陈旧构建产物的亏。");
    return out;
}

std::string WrapPromptWithTaskTemplate(const std::string& user_prompt) {
    return FormatText(ToolText("agent", "template.full",
                               "[宿主套壳·任务书六件套] 下面先录派工者任务原文,再附六件套核对单。六件套是引导"
                               "不是格式铁律:原文已写清的不必重抄,缺的项照提示补齐或向派工者问明,别自行脑补。"
                               "\n\n===== 任务原文 =====\n{0}\n===== 原文完 =====\n\n[六件套核对单]\n"
                               "1. 单子路径:本任务出自哪张单/哪段需求?原文没写就先问明,别猜。\n"
                               "2. 范围红线:只许动哪些文件/模块?哪些明确不碰?\n"
                               "3. 环境实情:宿主若在任务书尾部附了 [宿主注入·本机环境附录],照附录构建测试;没附就"
                               "动工前自己摸清本机构建环境,并在回报里写明。\n"
                               "4. 纪律:提交信息规矩、push 与否、版本号动不动、单子批次只勾真验证过的。\n"
                               "5. 完工标准:怎样算修完——测试全绿、回归零、验收命令过。\n"
                               "6. 回报格式:完工回报带分支、commit 号、测试汇总原文、落点(文件:行)、新增测试册。"),
                      {{"{0}", user_prompt}});
}

}  // namespace lubancode::tools
