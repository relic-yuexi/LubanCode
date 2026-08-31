// turn_capabilities.hpp 的实现:逐行拼段,零状态零 IO。

#include "app/turn_capabilities.hpp"

namespace lubancode::app {

namespace {

// 一行的正文:可用带注,不可用给"等相应轮次"的指路。文案固定,不带
// 轮次数字一类会逐轮变脸的内容——段随本轮消息走,变脸无害,但固定文案
// 更省 token 也更好认。
void AppendLine(std::string& out, const char* tool_name, const char* when_active, const TurnCapabilityLine& line) {
    if (!line.shown) {
        return;
    }
    out += "- ";
    out += tool_name;
    if (line.available) {
        out += ": 可用";
        if (!line.note.empty()) {
            out += "(当前 ";
            out += when_active;
            out += ":";
            out += line.note;
            out += ")";
        }
    } else {
        out += ": 不可用(当前不在 ";
        out += when_active;
        out += ";等相应轮次或换路径,不要在普通轮里调它)";
    }
    out += "\n";
}

}  // namespace

std::string BuildTurnCapabilitiesSegment(const TurnCapabilities& caps) {
    if (!caps.goal_checkpoint.shown && !caps.loop_control.shown) {
        return std::string();
    }
    std::string out;
    out += "\n[turn capabilities] 以下条件工具的定义常驻工具表,但只在对应轮次可执行;"
           "本段只是状态通报,执行门以真实轮次为准:\n";
    AppendLine(out, "goal_checkpoint", "goal 执行轮", caps.goal_checkpoint);
    AppendLine(out, "loop_control", "loop 定时拍", caps.loop_control);
    return out;
}

}  // namespace lubancode::app
