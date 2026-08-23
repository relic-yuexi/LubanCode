import io, os, re

# 从 /tmp/loop_head.cpp(HEAD 版,无冲突标记)重建 loop.cpp,
# 再织入 origin/main 的三个批次回调。
with io.open('/tmp/loop_head.cpp', 'rb') as f:
    head = f.read().decode('utf-8')

text = head

for inc in ['#include <cstdlib>', '#include <type_traits>', '#include <utility>', '#include <variant>']:
    if inc not in text:
        text = text.replace('#include <algorithm>\n', '#include <algorithm>\n' + inc + '\n', 1)

old = '''    int steps_used = 0;
    std::string last_stop_reason;
'''
new = '''    int steps_used = 0;
    std::string last_stop_reason;
    // 回合视觉收束(终端回合视觉收束单):本 Run() 已发出的批次序号
    // (on_tool_batch_started 的 batch_index 用;每个含工具的 step 消耗
    // 一枚,跨 step 不重号)。
    std::size_t batches_emitted = 0;
'''
assert old in text, "steps account"
text = text.replace(old, new, 1)

old = '''    for (int step_index = 0; profile_.max_steps_per_turn <= 0 || step_index < profile_.max_steps_per_turn; ++step_index) {
        // 跨会话传话的安全收件点:工具结果已攒完、下一次请求尚未发出——
'''
new = '''    for (int step_index = 0; profile_.max_steps_per_turn <= 0 || step_index < profile_.max_steps_per_turn; ++step_index) {
        // 回合视觉收束:step 边界。请求还没发,先报"这一拍开始了"——
        // 界面(工具批次分组)凭它知道上一批已换拍。没设回调零影响。
        if (callbacks.on_model_step_started) {
            callbacks.on_model_step_started(step_index);
        }
        // 跨会话传话的安全收件点:工具结果已攒完、下一次请求尚未发出——
'''
assert old in text, "step boundary"
text = text.replace(old, new, 1)

old = '''        bool interrupted = false;
        int tool_index = -1;
        std::vector<api::ContentBlock> tool_results;
'''
new = '''        // 回合视觉收束:批次边界。遍历前把这一批的 tool_use id 按模型给
        // 的顺序交出去(界面先全登记 Pending,再逐枚推进);遍历后(含
        // 打断补账)报收。没设回调零影响;执行语义一字不动。
        int batch_index_for_this_step = -1;
        {
            std::vector<std::string> batch_ids;
            for (const auto& block : assistant_message.content) {
                if (std::holds_alternative<api::ToolUseBlock>(block)) {
                    batch_ids.push_back(std::get<api::ToolUseBlock>(block).id);
                }
            }
            if (!batch_ids.empty() && callbacks.on_tool_batch_started) {
                callbacks.on_tool_batch_started(step_index, static_cast<int>(batches_emitted), batch_ids);
            }
            if (!batch_ids.empty()) {
                batch_index_for_this_step = static_cast<int>(batches_emitted);
                ++batches_emitted;
            }
        }
        bool interrupted = false;
        int tool_index = -1;
        std::vector<api::ContentBlock> tool_results;
'''
assert old in text, "batch start"
text = text.replace(old, new, 1)

old = '''        if (trace_armed) {
            // 批次尾:结果消息本体已入 history。先交装配层 append+flush user
'''
new = '''        if (batch_index_for_this_step >= 0 && callbacks.on_tool_batch_finished) {
            callbacks.on_tool_batch_finished(batch_index_for_this_step, interrupted);
        }

        if (trace_armed) {
            // 批次尾:结果消息本体已入 history。先交装配层 append+flush user
'''
assert old in text, "batch finished"
text = text.replace(old, new, 1)

with io.open('src/agent/loop.cpp', 'wb') as f:
    f.write(text.encode('utf-8'))
print("OK loop.cpp merged")
