// 工具文案语言档的端到端驱动(不进 ctest,验收/集成验证手动跑):
//   LUBANCODE_LANG=en ./tool_text_lang_driver
// 语言按真实启动链定:LUBANCODE_LANG(空 = 系统探测),与 cli_app.cpp
// 早初始化同一条路。定稿后把已迁移工具的 description 与 schema 参数说明
// 打到 stdout,外加 == 节名 == 定界标记,供逐字节比对:
//   - 缺省(中文机器)输出必须与迁移前逐字节一致;
//   - LUBANCODE_LANG=en 输出必须是英文档原文。
// 已迁移:read_file、write_file(试点),edit_file、search(文件工具批
// 余量),run_command、background_output、stop_background(命令族批)。

#include <iostream>
#include <string>

#include "cli/i18n.hpp"
#include "platform/paths.hpp"  // GetEnvVar:LUBANCODE_LANG,与 cli_app 早初始化同源
#include "tools/background_output.hpp"
#include "tools/edit_file.hpp"
#include "tools/read_file.hpp"
#include "tools/run_command.hpp"
#include "tools/search.hpp"
#include "tools/write_file.hpp"

int main() {
    std::string lang;
    if (const auto env = lubancode::platform::GetEnvVar("LUBANCODE_LANG"); env.has_value() && !env->empty()) {
        lang = *env;
    } else {
        lang = lubancode::cli::DetectSystemLanguage();
    }
    lubancode::cli::SetLanguage(lang);
    std::cerr << "[language] " << lang << "\n";

    lubancode::tools::ReadFileTool read;
    lubancode::tools::WriteFileTool write;

    std::cout << "== read_file.description ==\n" << read.description() << "\n";
    const nlohmann::json rs = read.input_schema();
    std::cout << "== read_file.param.path ==\n"
              << rs["properties"]["path"]["description"].get<std::string>() << "\n";
    std::cout << "== read_file.param.offset ==\n"
              << rs["properties"]["offset"]["description"].get<std::string>() << "\n";
    std::cout << "== read_file.param.limit ==\n"
              << rs["properties"]["limit"]["description"].get<std::string>() << "\n";
    std::cout << "== write_file.description ==\n" << write.description() << "\n";
    const nlohmann::json ws = write.input_schema();
    std::cout << "== write_file.param.path ==\n"
              << ws["properties"]["path"]["description"].get<std::string>() << "\n";
    std::cout << "== write_file.param.content ==\n"
              << ws["properties"]["content"]["description"].get<std::string>() << "\n";

    // 文件工具批余量(edit_file/search)与命令族批(run_command/
    // background_output/stop_background)。run_command 的 schema 随平台分档,
    // 输出自然带平台视角——比对时对同一平台取基准。
    lubancode::tools::EditFileTool edit;
    std::cout << "== edit_file.description ==\n" << edit.description() << "\n";
    const nlohmann::json es = edit.input_schema();
    for (const auto& item : es["properties"].items()) {
        std::cout << "== edit_file.param." << item.key() << " ==\n"
                  << item.value()["description"].get<std::string>() << "\n";
    }

    lubancode::tools::SearchTool search;
    std::cout << "== search.description ==\n" << search.description() << "\n";
    const nlohmann::json ss = search.input_schema();
    for (const auto& item : ss["properties"].items()) {
        std::cout << "== search.param." << item.key() << " ==\n"
                  << item.value()["description"].get<std::string>() << "\n";
    }

    lubancode::tools::RunCommandTool run;
    std::cout << "== run_command.description ==\n" << run.description() << "\n";
    const nlohmann::json rcs = run.input_schema();
    for (const auto& item : rcs["properties"].items()) {
        std::cout << "== run_command.param." << item.key() << " ==\n"
                  << item.value()["description"].get<std::string>() << "\n";
    }

    lubancode::tools::BackgroundOutputTool bg;
    std::cout << "== background_output.description ==\n" << bg.description() << "\n";
    const nlohmann::json bos = bg.input_schema();
    for (const auto& item : bos["properties"].items()) {
        std::cout << "== background_output.param." << item.key() << " ==\n"
                  << item.value()["description"].get<std::string>() << "\n";
    }

    lubancode::tools::StopBackgroundTool stop;
    std::cout << "== stop_background.description ==\n" << stop.description() << "\n";
    const nlohmann::json sbs = stop.input_schema();
    for (const auto& item : sbs["properties"].items()) {
        std::cout << "== stop_background.param." << item.key() << " ==\n"
                  << item.value()["description"].get<std::string>() << "\n";
    }
    return 0;
}
