// TurnItem(显示系统剥离单第五步:拆领域条目)。
//
// 中立领域条目:一轮问答里"发生了什么"的真值账——原始工具名、结构化
// input/result、状态、起止时间、diff 数据、错误码,一字不翻画面。终端把
// 它投影成 cli::TranscriptItem(现有 view model),Web/Tauri/app-server 直
// 接拿它造组件,两家吃同一份账,不各抄一遍。
//
// 与 cli::TranscriptItem 的分家线(单子"四、领域条目与前端 view model"):
//   - TranscriptItem 是终端 view model:标题、摘要行、截断全文、
//     steady_clock、状态灯文案;
//   - TurnItem 是领域模型:item_id、tool_use_id、原始 JSON、终态四分、
//     Unix epoch 毫秒、中立 diff 行表;
//   - 投影函数 ProjectTranscriptItem 在本头(runtime 层不该认 cli/*,
//     投影住 terminal 侧,见 terminal_turn_view.cpp 的 ProjectTranscript)。
//
// diff 中立行表(同单):计算移出 cli::ToolDisplay,产出 DiffRow(kind、
// old/new 行号、text、path)。终端按 DiffRow 添色画 ANSI,Web 按 kind 上
// DOM class,行号与正文两家一致。
//
// 依赖铁律同合同头:只认标准库与 nlohmann/json,不 include cli/app/
// frontend,零实现依赖。纯头文件。

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::runtime {

// ---------------------------------------------------------------------------
// 中立 diff 行表
// ---------------------------------------------------------------------------

// 一行 diff 的真值:上下文/删/增。不带 ANSI、不按宽截断、不拼行号栏——
// 那些是渲染层的活。终端添色,Web 添 DOM class,行号给两家自己排。
enum class DiffRowKind { Context, Del, Add };

struct DiffRow {
    DiffRowKind kind = DiffRowKind::Context;
    std::string text;
    int old_no = 0;  // 旧文件里的行号(1 起);Add 行没有,是 0
    int new_no = 0;  // 新文件里的行号(1 起);Del 行没有,是 0
};

// 一份完整 diff 的中立账:行表 + 事实摘要。头两行信息(header)不拼文案,
// 给足事实(路径、located、replaced_count、old 是否存在),前端自己措辞。
struct DiffTable {
    std::string path;                    // 目标文件(工具入参原样)
    std::vector<DiffRow> rows;
    bool located = true;                 // edit_file:old_string 在文件里找到了
    std::uint64_t replaced_count = 0;    // edit_file:预计替换几处
    bool old_exists = true;              // write_file:目标文件原已存在
    // edit_file 的段内回退(没找到 old_string)也照样给行表,前端不须特判。
    std::uint64_t added_lines() const;
    std::uint64_t removed_lines() const;
};

// edit_file / write_file 的入参 -> 中立行表;别的工具给 nullopt(与
// BuildFileDiffPreview 的取舍一致:只有这两个工具有"改动预览"的领域语义)。
// 读旧文件在这一层做(单子原文:diff 计算移出 ToolDisplay)——磁盘真值
// 是领域数据,不是画面数据。path 读不出/不存在按新文件处理,不因此崩。
std::optional<DiffTable> BuildDiffTable(const std::string& tool_name, const nlohmann::json& input);

// ---------------------------------------------------------------------------
// TurnItem:领域条目
// ---------------------------------------------------------------------------

// 终态四分(与 event.hpp 的 Outcome 同一套词):succeeded/failed/declined/
// cancelled。进行中不是终态,另立 Pending/Running 两枚前置态。
enum class TurnItemStatus { Pending, Running, Succeeded, Failed, Declined, Cancelled };

// 条目种类(与 event.hpp 的 ItemKind 同一套词的领域版):工具/思考/正文/
// 命令/diff/todo/子代理。
enum class TurnItemKind { Tool, SubTool, Thinking, Text, Command, Diff, Todo, Subagent };

// 完整输出的存储上限、协议发送上限、终端预览上限分三本账(单子原文)。
// 这里只定存储上限——领域账不许被终端 64KB 截断值反过来冒充(那是
// cli::kFullOutputCapBytes 的职责);发送/预览上限由各前端自管。
inline constexpr std::size_t kTurnItemOutputCapBytes = 256 * 1024;

// UTF-8 安全截断(不劈多字节字符)——与 cli::TruncateUtf8Bytes 同一套解码,
// runtime 侧独立一份,不 include cli/*。
std::string TruncateUtf8Bytes(const std::string& text, std::size_t max_bytes);

struct TurnItem {
    std::string item_id;      // IdAuthority 发的 item-<n>(空 = 终端旧路,投影兜底)
    std::string tool_use_id;  // 模型给的调用 id(ToolUseBlock.id / ptc-N;可空)
    TurnItemKind kind = TurnItemKind::Tool;
    std::string tool_name;    // 原始工具名(run_command / mcp__x__y / thinking / agent_notice)
    nlohmann::json input = nlohmann::json::object();  // 结构化入参(真值,不拼摘要)
    std::string result_text;     // 工具结果原文(截 kTurnItemOutputCapBytes)
    bool result_is_error = false;
    std::string error_code;      // 稳定错误码(失败态尽量给;空 = 没有稳定码)
    TurnItemStatus status = TurnItemStatus::Running;
    std::int64_t started_at_ms = 0;  // Unix epoch 毫秒
    std::int64_t ended_at_ms = 0;    // 同上;0 = 还没终态
    std::optional<DiffTable> diff;   // edit_file/write_file 的中立行表
    // 子代理条目附账:步数/子工具数(终态摘要的事实底子;前端拼文案)。
    int subagent_steps = 0;
    int subagent_tools = 0;

    bool finished() const {
        return status == TurnItemStatus::Succeeded || status == TurnItemStatus::Failed ||
               status == TurnItemStatus::Declined || status == TurnItemStatus::Cancelled;
    }
};

// 枚举 <-> 稳定字符串(线上是字符串;实现见 runtime_contract.cpp)。
std::string ToString(TurnItemStatus status);
std::string ToString(TurnItemKind kind);
bool ParseTurnItemStatus(const std::string& s, TurnItemStatus& out);
bool ParseTurnItemKind(const std::string& s, TurnItemKind& out);

}  // namespace lubancode::runtime
