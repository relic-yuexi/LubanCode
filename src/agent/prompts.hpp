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
           "给子代理去干,省着主对话的上下文。需要深读网页长文时同样交给子代理:先用 web_search 搜到"
           "线索,再用 web_fetch 抓正文,读完把结论带回来。遇到需要好几步才能完成的任务,先调用 todo_write 列一份"
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

// tool_search(延迟挂载)的索引段注入点:把"另有 N 个延迟工具……"那一段
// (tools::BuildDeferredToolsIndexSegment 算出来)追加到已拼好的系统提示
// 末尾;空串原样返回,一个字符都不多。跟 WithModelInstructions 同一个路数、
// 同一个理由:索引段随 loaded 集合逐轮变化(检索命中的工具要从索引里消失),
// AgentLoop 的系统提示构造后改不了,只能由发请求前的包装层(main.cpp 的
// DeferredIndexBackend)对 Request.system 现拼现用;子代理则在 AgentTool
// 构造 sub_loop 系统提示时按当下的 loaded 拼一次。
inline std::string WithDeferredToolsIndex(const std::string& system_prompt, const std::string& index_segment) {
    if (index_segment.empty()) {
        return system_prompt;
    }
    return system_prompt + "\n\n" + index_segment;
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

// ---------------------------------------------------------------------------
// 魂法分家(0.16.x):系统提示的"法"住 ~/.lubancode/system_prompt.md(内容
// 就是内置默认人格段的副本,用户改它定制行为),"魂"住 SOUL.md / souls/
// (风格叠加层,注入在系统提示最后)。两种文件顶部都带一行 HTML 注释说明
// 用途——注释是给人看的,不该喂给模型,注入前一律剥掉。
// 下面几个都是纯函数,不碰 IO,注入顺序、回退逻辑全在这儿,好单测。
// ---------------------------------------------------------------------------

// 剥掉文本里所有 <!-- ... --> 注释段(可跨行;没闭合的 <!-- 起,后面整段
// 都当注释丢掉),再剥两端空白。SOUL.md/system_prompt.md 注入前都过这一道。
inline std::string StripPromptComments(const std::string& text) {
    std::string out;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t open = text.find("<!--", pos);
        if (open == std::string::npos) {
            out += text.substr(pos);
            break;
        }
        out += text.substr(pos, open - pos);
        const std::size_t close = text.find("-->", open + 4);
        if (close == std::string::npos) {
            break;  // 注释没闭合,后面整段不要了
        }
        pos = close + 3;
    }
    std::size_t begin = 0;
    while (begin < out.size() && (out[begin] == ' ' || out[begin] == '\t' || out[begin] == '\r' || out[begin] == '\n')) {
        ++begin;
    }
    std::size_t end = out.size();
    while (end > begin && (out[end - 1] == ' ' || out[end - 1] == '\t' || out[end - 1] == '\r' || out[end - 1] == '\n')) {
        --end;
    }
    return out.substr(begin, end - begin);
}

// 法的裁决:--system-prompt 命令行参数(或配置的 system_prompt_file)读出
// 来的内容仍压过一切;没有 CLI 人格时用 system_prompt.md 的内容(剥掉注释;
// 文件缺失、或剥完全空白,返回空串——BuildSystemPrompt 收到空串自会回退
// 内置默认 DefaultPersona(),整条回退链就这么接上)。
inline std::string ResolvePersona(const std::string& cli_persona, const std::string& law_file_content) {
    if (!cli_persona.empty()) {
        return cli_persona;
    }
    return StripPromptComments(law_file_content);
}

// 魂的独立段:开头一行说明来源和边界,免得模型把风格叠加当成新人格、
// 或者拿它当借口不调用工具。
inline std::string SoulSegment(const std::string& soul_content) {
    return "风格叠加层(魂,来自 SOUL.md 或 souls/ 目录,只影响答话的语气风格;工具照常调用,内容照常准确):\n" +
           soul_content;
}

// 把魂叠加到一份已拼好的系统提示最后——永远是最后一段,压轴出场(调用处
// 保证在 WithModelInstructions 之后再调这个)。注释剥掉不注入;剥完全空白
// (SOUL.md 默认就只有一行注释)就原样返回,一个字符都不多。
inline std::string WithSoul(const std::string& system_prompt, const std::string& soul_content) {
    const std::string stripped = StripPromptComments(soul_content);
    if (stripped.empty()) {
        return system_prompt;
    }
    return system_prompt + "\n\n" + SoulSegment(stripped);
}

}  // namespace lubancode::agent
