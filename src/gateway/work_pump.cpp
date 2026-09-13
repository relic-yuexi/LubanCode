// job 控制命令文件的读写两侧(GatewayWorkPump 的合同口在头文件;本件
// 只装命令文件 IO)。stop.json 同款纪律:写侧原子落,读侧消费即删,
// 读不懂删掉留数,不追杀。
#include "gateway/work_pump.hpp"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iterator>

#include "platform/atomic_write.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"

namespace lubancode::gateway {

namespace {

std::atomic<std::uint64_t> g_command_seq{0};

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
    const auto write = platform::AtomicWriteFile(path, json.dump(),
                                                 platform::WriteDurability::AtomicVisibility);
    if (!write.has_value()) {
        return "写命令文件失败(" + write.error().code + "): " +
               platform::PathToUtf8(path);
    }
    return std::string();
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

std::string WriteJobAddCommand(const std::filesystem::path& control_dir,
                               const GatewayJobAddCommand& command) {
    return WriteCommandFile(CommandFilePath(control_dir, "add"), command.ToJson());
}

std::string WriteJobRunNowCommand(const std::filesystem::path& control_dir,
                                  const GatewayJobRunNowCommand& command) {
    return WriteCommandFile(CommandFilePath(control_dir, "run-now"), command.ToJson());
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
        std::ifstream stream(file, std::ios::binary);
        nlohmann::json parsed;
        bool ok = false;
        if (stream) {
            const std::string text((std::istreambuf_iterator<char>(stream)),
                                   std::istreambuf_iterator<char>());
            try {
                parsed = nlohmann::json::parse(text);
                ok = true;
            } catch (const nlohmann::json::exception&) {
                ok = false;
            }
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
                consumed.adds.push_back(std::move(command));
                handled = true;
            } else if (type == "job.run_now" && !get_string("jobId").empty()) {
                GatewayJobRunNowCommand command;
                command.job_id = get_string("jobId");
                command.idempotency_key = get_string("idempotencyKey");
                command.requested_at_ms = get_int("requestedAtMs");
                consumed.run_nows.push_back(std::move(command));
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
