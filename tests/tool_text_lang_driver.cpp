// 工具文案语言档的端到端驱动(不进 ctest,验收/集成验证手动跑):
//   LUBANCODE_LANG=en ./tool_text_lang_driver
// 语言按真实启动链定:LUBANCODE_LANG(空 = 系统探测),与 cli_app.cpp
// 早初始化同一条路。定稿后把试点工具的 description 与 schema 参数说明
// 打到 stdout,外加 == 节名 == 定界标记,供逐字节比对:
//   - 缺省(中文机器)输出必须与迁移前逐字节一致;
//   - LUBANCODE_LANG=en 输出必须是英文档原文。

#include <iostream>
#include <string>

#include "cli/i18n.hpp"
#include "platform/paths.hpp"  // GetEnvVar:LUBANCODE_LANG,与 cli_app 早初始化同源
#include "tools/read_file.hpp"
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
    return 0;
}
