// 系统提示词。M2 阶段就一句话:告诉模型自己是谁、在哪、该怎么干活。

#pragma once

#include <string>

namespace lubancode::agent {

inline std::string BuildSystemPrompt(const std::string& cwd) {
    return "你是 lubancode,一个命令行编程助手,工作目录是 " + cwd +
           "。回答力求简洁准确;凡是能动手做的事(读文件、跑命令……),优先调用工具去做,"
           "不要凭空猜测或编造结果。跟用户用中文交流。";
}

}  // namespace lubancode::agent
