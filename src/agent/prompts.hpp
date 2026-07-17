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
    return "你是 lubancode,一个命令行编程助手,跟用户用中文交流,回答力求简洁准确。遇到需要大量翻找"
           "文件、通读多份文件才能得出结论,而结论本身只需一小段的任务,优先用 agent 工具把这类活委托"
           "给子代理去干,省着主对话的上下文。遇到需要好几步才能完成的任务,先调用 todo_write 列一份"
           "清单,每做完一步就整表更新一次对应项的状态,让用户看得见进度。";
}

inline std::string EnvironmentSegment(const std::string& cwd) {
    return "工作目录是 " + cwd +
           "。凡是能动手做的事(读文件、跑命令、改文件……),优先调用工具去做,不要凭空猜测或编造结果——"
           "这条不受上面人格设定的影响,该用工具时就用。";
}

// 模型目录(models.json)base_instructions 的独立段:既不是人格段(不被
// --system-prompt 替换),也不是环境段本身,而是跟着"当前模型"走的又一段
// ——切模型就换、切到目录外模型就没有。开头一行说明来源,免得模型把它
// 跟人格设定混为一谈。
inline std::string ModelInstructionsSegment(const std::string& base_instructions) {
    return "模型专属指令(来自模型目录 models.json 的 base_instructions,随当前模型生效):\n" + base_instructions;
}

// 把模型专属段追加到一份已拼好的系统提示末尾;base_instructions 为空就
// 原样返回,一个字符都不多。之所以单独给这个函数、不并进 BuildSystemPrompt
// 的参数表:/model 切换要"下一轮请求生效",而 AgentLoop 的系统提示构造后
// 改不了(agent 层现有文件不动),只能由发请求前的包装层(main.cpp 的
// ModelInstructionsBackend)对 Request.system 现拼现用——单发模式没有
// /model,构造时直接用这个函数拼一次也是同一份结构。
inline std::string WithModelInstructions(const std::string& system_prompt, const std::string& base_instructions) {
    if (base_instructions.empty()) {
        return system_prompt;
    }
    return system_prompt + "\n\n" + ModelInstructionsSegment(base_instructions);
}

// custom_persona 留空(默认)时用 DefaultPersona();非空时整段替换人格,
// 环境段照样追加在后面,不会被换掉。
// skills_segment(M9 新增):tools::BuildSkillsPromptSegment() 算出来的
// "可用技能" 那一段,留空(默认,没有技能时也是空串)就不追加——不占
// 一个字符,不影响没配技能的既有场景。放在环境段之后,同样不受人格设定
// 影响(工具该用的时候还是要用,技能该加载的时候还是要加载)。
inline std::string BuildSystemPrompt(const std::string& cwd, const std::string& custom_persona = std::string(),
                                      const std::string& skills_segment = std::string()) {
    const std::string persona = custom_persona.empty() ? DefaultPersona() : custom_persona;
    std::string prompt = persona + "\n\n" + EnvironmentSegment(cwd);
    if (!skills_segment.empty()) {
        prompt += "\n\n" + skills_segment;
    }
    return prompt;
}

}  // namespace lubancode::agent
