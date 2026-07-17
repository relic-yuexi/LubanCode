// 系统提示词。拆成两段:
//   人格段(persona)—— 定义"模型是谁、该用什么语气说话"。--system-prompt
//   命令行参数 / 配置文件 system_prompt_file 字段换的就是这一段,不给就用
//   DefaultPersona()。
//   环境段(environment)—— 运行必需的上下文:工作目录、工具调用这条硬
//   规矩。不管人格段换没换、换成什么样,这一段永远原样追加在后面——
//   自定义人格哪怕定得再出格(比如"只用文言文回答"),工具该调用的时候
//   还是要调用,不受人格设定影响。

#pragma once

#include <string>

namespace lubancode::agent {

inline std::string DefaultPersona() {
    return "你是 lubancode,一个命令行编程助手,跟用户用中文交流,回答力求简洁准确。";
}

inline std::string EnvironmentSegment(const std::string& cwd) {
    return "工作目录是 " + cwd +
           "。凡是能动手做的事(读文件、跑命令、改文件……),优先调用工具去做,不要凭空猜测或编造结果——"
           "这条不受上面人格设定的影响,该用工具时就用。";
}

// custom_persona 留空(默认)时用 DefaultPersona();非空时整段替换人格,
// 环境段照样追加在后面,不会被换掉。
inline std::string BuildSystemPrompt(const std::string& cwd, const std::string& custom_persona = std::string()) {
    const std::string persona = custom_persona.empty() ? DefaultPersona() : custom_persona;
    return persona + "\n\n" + EnvironmentSegment(cwd);
}

}  // namespace lubancode::agent
