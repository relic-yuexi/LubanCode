// job 控制命令文件的读写两侧(GatewayWorkPump 的合同口在头文件;本件
// 只装命令文件 IO)。stop.json 同款纪律:写侧原子落(Windows 换名瞬拒
// 有界重试),读侧消费即删——读不懂(内容坏)删掉留数不追杀;打不开
// (Windows 换名后的过滤驱动短拒窗)内容未知,留待下一拍,不删。
#include "gateway/work_pump.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <thread>

#include "platform/atomic_write.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"

namespace lubancode::gateway {

namespace {

std::atomic<std::uint64_t> g_command_seq{0};

// Windows 换名瞬拒的有界重试档(与 workspace/manifest.cpp 同款纪律):原子
// 换名(MoveFileExW REPLACE)与防病毒/索引过滤驱动会让目标文件的打开
// 被短拒数十毫秒(该件三案 CI 实测:写侧 320 次换名被拒 48-57 次)。读
// 侧同窗照打在刚落地的命令文件上——读侧重试靠泵的轮询节拍(文件留在
// 原地下一拍再试,见 PollJobCommands);写侧重试在此处,10 次×10ms 预算
// 给足余量。POSIX rename 原子、无共享违例,重试路径零开销零行为变化。
constexpr int kTransientWriteAttempts = 10;
constexpr std::chrono::milliseconds kTransientWriteBackoff{10};

// 一枚命令文件名:job-add-<pid>-<seq>.json / job-run-now-<pid>-<seq>.json。
// 文件名只防互踩,不承载语义。
std::filesystem::path CommandFilePath(const std::filesystem::path& control_dir,
                                      const char* verb) {
    const std::uint64_t seq = g_command_seq.fetch_add(1);
    return control_dir / ("job-" + std::string(verb) + "-" +
                          std::to_string(platform::CurrentProcessId()) + "-" +
                          std::to_string(seq) + ".json");
}

std::string WriteCommandFile(const std::filesystem::path& path, const nlohmann::json& json) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec && !path.parent_path().empty()) {
        return "建控制目录失败: " + ec.message();
    }
    // 原子换名的瞬态拒(依据见 kTransientWrite* 注释)有界重试:命令丢了
    // 上游只剩 15 秒回执超时一路红(受理幂等键在,重发不双建,但这一单
    // 就红了)。正常路径首次即成,零等待零重试。
    for (int attempt = 0;; ++attempt) {
        const auto write = platform::AtomicWriteFile(path, json.dump(),
                                                     platform::WriteDurability::AtomicVisibility);
        if (write.has_value()) {
            return std::string();
        }
        if (attempt >= kTransientWriteAttempts) {
            return "写命令文件失败(" + write.error().code + "): " +
                   platform::PathToUtf8(path);
        }
        std::this_thread::sleep_for(kTransientWriteBackoff);
    }
}

}  // namespace

// 计划载荷的写读两侧共用:出现即写键,读侧按键出现回填 set_*。
namespace {

nlohmann::json SchedulePatchToJson(const GatewayJobSchedulePatch& patch) {
    nlohmann::json json = nlohmann::json::object();
    if (patch.set_due_at) json["dueAtMs"] = patch.due_at_ms;
    if (patch.set_interval) json["intervalSeconds"] = patch.interval_seconds;
    if (patch.set_cron) json["cronExpr"] = patch.cron_expr;
    if (patch.set_timezone) json["timezone"] = patch.timezone;
    if (patch.set_misfire) json["misfirePolicy"] = patch.misfire;
    if (patch.set_deadline) json["deadlineMs"] = patch.deadline_ms;
    if (patch.set_notify_on_change) json["notifyOnChange"] = patch.notify_on_change;
    return json;
}

GatewayJobSchedulePatch SchedulePatchFromJson(const nlohmann::json& json) {
    GatewayJobSchedulePatch patch;
    auto has = [&json](const char* key) { return json.is_object() && json.contains(key); };
    if (has("dueAtMs") && json["dueAtMs"].is_number_integer()) {
        patch.set_due_at = true;
        patch.due_at_ms = json["dueAtMs"].get<std::int64_t>();
    }
    if (has("intervalSeconds") && json["intervalSeconds"].is_number_integer()) {
        patch.set_interval = true;
        patch.interval_seconds = json["intervalSeconds"].get<std::int64_t>();
    }
    if (has("cronExpr") && json["cronExpr"].is_string()) {
        patch.set_cron = true;
        patch.cron_expr = json["cronExpr"].get<std::string>();
    }
    if (has("timezone") && json["timezone"].is_string()) {
        patch.set_timezone = true;
        patch.timezone = json["timezone"].get<std::string>();
    }
    if (has("misfirePolicy") && json["misfirePolicy"].is_string()) {
        patch.set_misfire = true;
        patch.misfire = json["misfirePolicy"].get<std::string>();
    }
    if (has("deadlineMs") && json["deadlineMs"].is_number_integer()) {
        patch.set_deadline = true;
        patch.deadline_ms = json["deadlineMs"].get<std::int64_t>();
    }
    if (has("notifyOnChange") && json["notifyOnChange"].is_boolean()) {
        patch.set_notify_on_change = true;
        patch.notify_on_change = json["notifyOnChange"].get<bool>();
    }
    return patch;
}

}  // namespace

nlohmann::json GatewayJobAddCommand::ToJson() const {
    nlohmann::json json = nlohmann::json::object();
    json["type"] = "job.add";
    json["schemaVersion"] = schema_version;
    json["prompt"] = prompt;
    json["idempotencyKey"] = idempotency_key;
    if (!job_id.empty()) json["jobId"] = job_id;
    json["dueAtMs"] = due_at_ms;
    json["requestedAtMs"] = requested_at_ms;
    // V2 计划键:出现即写(--every/--cron 等由 CLI 折进 schedule)。
    const nlohmann::json patch = SchedulePatchToJson(schedule);
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        json[it.key()] = it.value();
    }
    return json;
}

nlohmann::json GatewayJobRunNowCommand::ToJson() const {
    nlohmann::json json = nlohmann::json::object();
    json["type"] = "job.run_now";
    json["schemaVersion"] = schema_version;
    json["jobId"] = job_id;
    json["idempotencyKey"] = idempotency_key;
    json["requestedAtMs"] = requested_at_ms;
    return json;
}

nlohmann::json GatewayJobUpdateCommand::ToJson() const {
    nlohmann::json json = nlohmann::json::object();
    json["type"] = "job.update";
    json["schemaVersion"] = schema_version;
    json["jobId"] = job_id;
    json["expectedRevision"] = expected_revision;
    json["idempotencyKey"] = idempotency_key;
    if (!prompt.empty()) json["prompt"] = prompt;
    json["requestedAtMs"] = requested_at_ms;
    const nlohmann::json patch = SchedulePatchToJson(schedule);
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        json[it.key()] = it.value();
    }
    return json;
}

nlohmann::json GatewayJobStateCommand::ToJson() const {
    nlohmann::json json = nlohmann::json::object();
    json["type"] = "job." + verb;
    json["schemaVersion"] = schema_version;
    json["jobId"] = job_id;
    json["expectedRevision"] = expected_revision;
    json["idempotencyKey"] = idempotency_key;
    json["requestedAtMs"] = requested_at_ms;
    return json;
}

nlohmann::json GatewayJobImportLoopCommand::ToJson() const {
    nlohmann::json json = nlohmann::json::object();
    json["type"] = "job.import_loop";
    json["schemaVersion"] = schema_version;
    json["sourceSessionId"] = source_session_id;
    json["sourceTaskId"] = source_task_id;
    json["prompt"] = prompt;
    json["intervalSeconds"] = interval_seconds;
    json["idempotencyKey"] = idempotency_key;
    json["requestedAtMs"] = requested_at_ms;
    return json;
}

std::string WriteJobAddCommand(const std::filesystem::path& control_dir,
                               const GatewayJobAddCommand& command) {
    return WriteCommandFile(CommandFilePath(control_dir, "add"), command.ToJson());
}

std::string WriteJobRunNowCommand(const std::filesystem::path& control_dir,
                                  const GatewayJobRunNowCommand& command) {
    return WriteCommandFile(CommandFilePath(control_dir, "run-now"), command.ToJson());
}

std::string WriteJobUpdateCommand(const std::filesystem::path& control_dir,
                                  const GatewayJobUpdateCommand& command) {
    return WriteCommandFile(CommandFilePath(control_dir, "update"), command.ToJson());
}

std::string WriteJobStateCommand(const std::filesystem::path& control_dir,
                                 const GatewayJobStateCommand& command) {
    // 文件名的 verb 段(pause/resume/cancel);内容里的 type 同款。
    return WriteCommandFile(CommandFilePath(control_dir, command.verb.c_str()),
                            command.ToJson());
}

std::string WriteJobImportLoopCommand(const std::filesystem::path& control_dir,
                                      const GatewayJobImportLoopCommand& command) {
    return WriteCommandFile(CommandFilePath(control_dir, "import-loop"), command.ToJson());
}

ConsumedJobCommands PollJobCommands(const std::filesystem::path& control_dir) {
    ConsumedJobCommands consumed;
    std::error_code ec;
    if (!std::filesystem::exists(control_dir, ec) || ec) {
        return consumed;  // 没有控制目录 = 没有命令,零副作用
    }
    // 文件名序消费(写入侧带 pid+seq,字典序近似到达序;单写者下够用)。
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(control_dir, ec)) {
        const std::string name = entry.path().filename().generic_string();
        if (name.rfind("job-", 0) == 0 && name.size() > 5 && name.substr(name.size() - 5) == ".json") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    for (const std::filesystem::path& file : files) {
        // 读与删分段:MSVC 的 ifstream 句柄不带 FILE_SHARE_DELETE(fstream
        // 长期缺口),stream 开着调 remove 必吃共享违例——旧码在 stream
        // 存活期里删,windows 腿上删除静默失败,命令文件每拍重消费,靠
        // store 幂等兜底白烧。先读完、关柄,再删。
        std::string text;
        {
            std::ifstream stream(file, std::ios::binary);
            if (!stream) {
                // 打不开≠读不懂:Windows 上原子换名落地的短窗里,防病毒/
                // 索引过滤驱动会把目标文件的打开短拒数十毫秒(manifest.cpp
                // 三案 CI 实测的同款病灶)。内容没读着就不能按坏数据处置:
                // 文件留在原地,泵的轮询节拍就是重试;真打不开也只是每拍
                // 一次空探,零副作用。
                continue;
            }
            text.assign((std::istreambuf_iterator<char>(stream)),
                        std::istreambuf_iterator<char>());
        }
        nlohmann::json parsed;
        bool ok = false;
        try {
            parsed = nlohmann::json::parse(text);
            ok = true;
        } catch (const nlohmann::json::exception&) {
            ok = false;
        }
        bool handled = false;
        if (ok && parsed.is_object() && parsed.contains("type") && parsed["type"].is_string()) {
            const std::string type = parsed["type"].get<std::string>();
            const auto get_string = [&parsed](const char* key) {
                return parsed.contains(key) && parsed[key].is_string()
                           ? parsed[key].get<std::string>()
                           : std::string();
            };
            const auto get_int = [&parsed](const char* key) -> std::int64_t {
                return parsed.contains(key) && parsed[key].is_number_integer()
                           ? parsed[key].get<std::int64_t>()
                           : 0;
            };
            if (type == "job.add" && parsed.contains("prompt") && parsed["prompt"].is_string() &&
                !parsed["prompt"].get<std::string>().empty()) {
                GatewayJobAddCommand command;
                command.prompt = parsed["prompt"].get<std::string>();
                command.idempotency_key = get_string("idempotencyKey");
                command.job_id = get_string("jobId");
                command.due_at_ms = get_int("dueAtMs");
                command.requested_at_ms = get_int("requestedAtMs");
                command.schedule = SchedulePatchFromJson(parsed);
                consumed.adds.push_back(std::move(command));
                handled = true;
            } else if (type == "job.run_now" && !get_string("jobId").empty()) {
                GatewayJobRunNowCommand command;
                command.job_id = get_string("jobId");
                command.idempotency_key = get_string("idempotencyKey");
                command.requested_at_ms = get_int("requestedAtMs");
                consumed.run_nows.push_back(std::move(command));
                handled = true;
            } else if (type == "job.update" && !get_string("jobId").empty() &&
                       get_int("expectedRevision") > 0) {
                GatewayJobUpdateCommand command;
                command.job_id = get_string("jobId");
                command.expected_revision =
                    static_cast<std::uint64_t>(get_int("expectedRevision"));
                command.idempotency_key = get_string("idempotencyKey");
                command.prompt = get_string("prompt");
                command.requested_at_ms = get_int("requestedAtMs");
                command.schedule = SchedulePatchFromJson(parsed);
                consumed.updates.push_back(std::move(command));
                handled = true;
            } else if ((type == "job.pause" || type == "job.resume" || type == "job.cancel") &&
                       !get_string("jobId").empty() && get_int("expectedRevision") > 0) {
                GatewayJobStateCommand command;
                command.verb = type.substr(4);  // "job." 之后
                command.job_id = get_string("jobId");
                command.expected_revision =
                    static_cast<std::uint64_t>(get_int("expectedRevision"));
                command.idempotency_key = get_string("idempotencyKey");
                command.requested_at_ms = get_int("requestedAtMs");
                consumed.state_ops.push_back(std::move(command));
                handled = true;
            } else if (type == "job.import_loop" && !get_string("sourceSessionId").empty() &&
                       !get_string("sourceTaskId").empty() &&
                       parsed.contains("prompt") && parsed["prompt"].is_string() &&
                       !parsed["prompt"].get<std::string>().empty() &&
                       get_int("intervalSeconds") > 0) {
                GatewayJobImportLoopCommand command;
                command.source_session_id = get_string("sourceSessionId");
                command.source_task_id = get_string("sourceTaskId");
                command.prompt = parsed["prompt"].get<std::string>();
                command.interval_seconds = get_int("intervalSeconds");
                command.idempotency_key = get_string("idempotencyKey");
                command.requested_at_ms = get_int("requestedAtMs");
                consumed.import_loops.push_back(std::move(command));
                handled = true;
            }
        }
        if (!handled) {
            ++consumed.discarded;
        }
        // 消费即删(读不懂也删——陈旧/坏命令不追杀,同 stop.json 口径)。
        std::error_code remove_ec;
        std::filesystem::remove(file, remove_ec);
    }
    return consumed;
}

}  // namespace lubancode::gateway
