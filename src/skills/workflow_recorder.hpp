// 录一遍生成技能(0.25.x):终端内示范的录制流水。
//
// 用户 /record start 明着开录之后,这一场会话里的目标口述、模型工具调用
// (脱敏后的入参与短摘要)、备注、验证结果,逐条追加写进
//   <主目录>/.lubancode/recordings/<录制id>/{manifest.json,events.jsonl}
// 停止后由 skill_drafter 归纳成 draft/SKILL.md,确认后经
// config::InstallDraftSkill 原子装进 skills 目录。目录与录制件分开,不污染
// 会话 JSONL。
//
// 分层:事件序列化/解析、脱敏、状态机迁移表全是纯函数,单测钉死;
// WorkflowRecorder 是碰磁盘的落盘句柄(append+flush,崩溃安全——半截行
// 由 ReadRecordingEvents 跳过,半截录制件没有 draft 就装不进 skills)。
// 密钥这道门在这里把:入盘前一律过 SanitizeToolInput/RedactSecrets,
// 工具结果的原始大输出不收,只留首行短摘要(原始输出仍在会话存档里)。
//
// 必须用户明着开录:本模块没有任何自动续录入口;进程重启后旧录制件只
// 能被"列出来/丢弃/补装草稿",不会被悄悄接着录。

#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::agent {

// ---------------------------------------------------------------------------
// 状态机(纯)
// ---------------------------------------------------------------------------

enum class RecorderState { Inactive, Recording, Paused };
enum class RecorderAction { Start, Pause, Resume, Stop, Cancel };

// 合法迁移:Inactive+Start;Recording+Pause/Stop/Cancel;Paused+Resume/Stop/Cancel。
// 其余(含 Inactive 下任何动作)一律非法——非法迁移由命令层打提示,不改状态。
bool IsValidRecorderTransition(RecorderState state, RecorderAction action);

// ---------------------------------------------------------------------------
// 事件(纯数据 + 一行 JSON 序列化)
// ---------------------------------------------------------------------------

// events.jsonl 认的事件类型(type 字段的取值)。
inline constexpr const char* kEventRecordStart = "record_start";
inline constexpr const char* kEventGoal = "goal";
inline constexpr const char* kEventVariable = "variable";
inline constexpr const char* kEventUserNote = "user_note";
inline constexpr const char* kEventToolCall = "tool_call";
inline constexpr const char* kEventToolResult = "tool_result";
inline constexpr const char* kEventVerification = "verification";
inline constexpr const char* kEventPause = "pause";
inline constexpr const char* kEventResume = "resume";
inline constexpr const char* kEventRecordStop = "record_stop";

struct RecordEvent {
    std::int64_t seq = 0;   // 顺序号,录制件内从 1 起单调递增
    std::string ts;         // "yyyy-mm-dd HH:MM:SS",落盘时刻
    std::string source;     // "user" / "model" / "system"
    std::string type;       // kEvent* 之一
    nlohmann::json data = nlohmann::json::object();
};

// 事件 -> 一行 JSON(不带换行符):{"seq":..,"ts":..,"source":..,"type":..,"data":{..}}。
std::string SerializeRecordEvent(const RecordEvent& event);

// 一行 JSON -> 事件。不是合法 JSON、缺 seq/ts/source/type、或 data 不是
// object,给 nullopt——坏行调用方跳过(半截录制件不废,丢弃或续读都行)。
std::optional<RecordEvent> ParseRecordEvent(const std::string& line);

// ---------------------------------------------------------------------------
// 脱敏(纯函数,密钥这道门)
// ---------------------------------------------------------------------------

// 值形态打码:文本里 authorization/cookie/token/api_key/password/passwd/
// secret 这类键跟着 ":" 或 "=" 的赋值,值换成 "[已打码]";值以 bearer 起头
// 的连掩一词;bearer 与 "sk-" 起头的 key 形态 token 整体打码。大小写不敏感。
std::string RedactSecrets(std::string text);

// 工具入参脱敏:递归走一遍 JSON——字段名(小写化、'-' 归成 '_' 后)含
// token/secret/password/passwd/authorization/cookie/api_key/apikey/
// private_key 的字段,值整体换成 "[已打码]";其余字符串值过一遍
// RedactSecrets,并截到 1000 字符(长参数只留前段)。数组逐项同规矩。
nlohmann::json SanitizeToolInput(const nlohmann::json& input);

// ---------------------------------------------------------------------------
// 录制件命名与磁盘薄壳
// ---------------------------------------------------------------------------

// 录制件名 slug:ASCII 字母数字与 '-' '_' '.' 原样留,中文等多字节按
// MakeSessionSlug 同款规矩逐码点清洗(空白与文件名危险字符换 '-',连续并
// 一,首尾剥掉),全剥没了给 "recording"。返回值保证能当目录名。
std::string MakeRecordingSlug(const std::string& name, std::size_t max_chars = 24);

struct RecordingStartInfo {
    std::string name;                     // 用户给的名字(任意 UTF-8)
    std::string goal;                     // 这桩活最后要得什么
    std::vector<std::string> variables;   // 哪些值每回都会变(口述)
    std::string acceptance;               // 看见什么才算做成
    std::string cwd;                      // 开录时的 cwd(UTF-8)
};

// 一场录制的落盘句柄。Start() 建目录、写 manifest.json、追加 record_start
// (含 goal/variables/acceptance 口述,起草时全从事件里取,manifest 只作
// 簿记)。此后每条事件 append+flush;写失败置 broken,后续写入变空操作
// (打一次警告),不拦着会话本身。
class WorkflowRecorder {
public:
    // 开录。recordings_root 由调用方算好传入(通常 <主目录>/.lubancode/
    // recordings);建不出目录/开不出文件返回错误。
    static std::expected<WorkflowRecorder, std::string> Start(const std::filesystem::path& recordings_root,
                                                              const RecordingStartInfo& info);

    // 暂停/续录:各追加一条 pause/resume 事件。状态不对返回错误,不动文件。
    std::expected<void, std::string> Pause();
    std::expected<void, std::string> Resume();

    // 收尾:先补一条 verification(最后一次验证结果,可为空),再追加
    // record_stop、关流。返回录制件目录,交给起草器。
    std::expected<std::filesystem::path, std::string> Stop(const std::string& final_verification);

    // 取消:关流并删掉整场录制件目录。已装好的 skill 不归它管,自然不动。
    std::expected<void, std::string> Cancel();

    // 用户备注(Recording 与 Paused 态都收——暂停时想到一句补进去也合理)。
    std::expected<void, std::string> Note(const std::string& text);

    // 工具事件(model 来源)。只在 Recording 态生效;Paused 静默跳过(不录
    // 暂停期间的动作),Inactive 是调用方没挂监听,天然到不了这里。
    // 逐枚追踪单:改带 execution_id/tool_use_id——同名五连可配对,录制件
    // 从 canonical tool trace 派生,不由 TurnRunner 再手打一遍事件。旧参
    // 数位置不动,新参末尾带默认值,既有调用方照旧编译。
    void RecordToolCall(const std::string& tool_name, const nlohmann::json& input,
                        const std::string& execution_id = std::string(),
                        const std::string& tool_use_id = std::string());
    void RecordToolResult(const std::string& tool_name, bool is_error, const std::string& content,
                          const std::string& outcome = std::string(),
                          const std::string& error_code = std::string(),
                          const std::string& execution_id = std::string());

    RecorderState state() const { return state_; }
    const std::string& name() const { return name_; }
    const std::string& id() const { return id_; }
    std::filesystem::path dir() const { return dir_; }
    bool broken() const { return broken_; }  // 落盘失败过,录制件不完整

private:
    WorkflowRecorder(std::filesystem::path dir, std::string name, std::string id, std::ofstream out);

    void AppendEvent(const char* source, const char* type, nlohmann::json data);

    std::filesystem::path dir_;
    std::string name_;
    std::string id_;
    std::ofstream out_;
    RecorderState state_ = RecorderState::Recording;
    std::int64_t next_seq_ = 1;
    bool broken_ = false;
};

// ---------------------------------------------------------------------------
// 录制件盘点 / 崩溃恢复
// ---------------------------------------------------------------------------

struct RecordingStatus {
    std::string id;                  // 目录名
    std::string name;                // manifest 里的名字,读不到就用 id
    std::filesystem::path dir;
    std::string started_at;          // manifest 里读,读不到空着
    bool finished = false;           // events.jsonl 里有 record_stop
    bool has_draft = false;          // draft/SKILL.md 在(装得进 skills 的前提)
};

// 扫 recordings_root 下的录制件目录(有 events.jsonl 或 manifest.json 的算)。
// 目录不存在给空表。按 id 倒序(id 以时间戳起头,字典倒序即时间倒序)。
std::vector<RecordingStatus> ListRecordings(const std::filesystem::path& recordings_root);

// 整读一场录制件的事件流。坏行/半截行(崩溃截断)跳过,不废整场。
std::vector<RecordEvent> ReadRecordingEvents(const std::filesystem::path& recording_dir);

// 丢弃一场录制件:删整个目录。id 只认单段目录名(含斜杠/.. 一律拒绝)。
std::expected<void, std::string> DiscardRecording(const std::filesystem::path& recordings_root,
                                                  const std::string& id);

}  // namespace lubancode::agent
