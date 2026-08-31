#include "channel/account_lock.hpp"

#include <array>
#include <fstream>

#include "platform/paths.hpp"
#include "platform/process.hpp"

namespace lubancode::channel {

namespace {

std::optional<AccountLockRecord> ReadLockFile(const std::filesystem::path& lock_file,
                                              std::string* error) {
    std::error_code ec;
    if (!std::filesystem::exists(lock_file, ec) || ec) {
        return std::nullopt;  // 没有锁文件
    }
    std::ifstream stream(lock_file, std::ios::binary);
    if (!stream) {
        if (error != nullptr) *error = "锁文件在,但打不开";
        return std::nullopt;
    }
    std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    // 注意:istreambuf_iterator 读到尾不置 eofbit,不能拿 stream.eof() 判
    // "读全了";空不空只看文本本身。
    if (text.empty()) {
        if (error != nullptr) *error = "锁文件是空的";
        return std::nullopt;
    }
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(text);
    } catch (const nlohmann::json::exception&) {
        if (error != nullptr) *error = "锁文件不是合法 JSON(可能写了一半)";
        return std::nullopt;
    }
    std::string parse_error;
    auto record = AccountLockRecord::FromJsonStrict(parsed, &parse_error);
    if (!record.has_value()) {
        if (error != nullptr) *error = "锁文件账读不懂: " + parse_error;
        return std::nullopt;
    }
    return record;
}

// 写锁文件。取舍:直接截断写,不做 temp+rename 原子换——锁的对手是
// 本机另一只实例的启动竞态(毫秒级),不是断电一致性;半写坏锁走
// RefusedBrokenLock 留给人看,不静默当作可抢。configuration.md §11 钉的
// 是"核进程存活再清",不是原子写。
bool WriteLockFile(const std::filesystem::path& lock_file, const AccountLockRecord& record) {
    std::error_code ec;
    std::filesystem::create_directories(lock_file.parent_path(), ec);
    if (ec && !lock_file.parent_path().empty()) return false;
    std::ofstream stream(lock_file, std::ios::binary | std::ios::trunc);
    if (!stream) return false;
    const std::string text = record.ToJson().dump();
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.flush();
    return static_cast<bool>(stream);
}

}  // namespace

AccountLock::AliveChecker AccountLock::DefaultAliveChecker() {
    return [](unsigned long pid) { return platform::IsProcessAlive(pid); };
}

nlohmann::json AccountLockRecord::ToJson() const {
    nlohmann::json json = nlohmann::json::object();
    json["pid"] = pid;
    json["start_time_ms"] = start_time_ms;
    json["acquired_at_ms"] = acquired_at_ms;
    json["generation"] = generation;
    json["instance_token"] = instance_token;
    return json;
}

std::optional<AccountLockRecord> AccountLockRecord::FromJsonStrict(const nlohmann::json& json,
                                                                   std::string* error) {
    const auto fail = [error](const std::string& message) {
        if (error != nullptr) *error = message;
        return std::nullopt;
    };
    if (!json.is_object()) return fail("锁账必须是 JSON object");
    const std::array<const char*, 5> kRequired = {"pid", "start_time_ms", "acquired_at_ms",
                                                  "generation", "instance_token"};
    for (const char* key : kRequired) {
        if (!json.contains(key)) return fail(std::string("缺必填字段 ") + key);
    }
    for (auto it = json.begin(); it != json.end(); ++it) {
        bool known = false;
        for (const char* key : kRequired) {
            if (it.key() == key) {
                known = true;
                break;
            }
        }
        if (!known) return fail("未知字段 " + it.key());
    }
    if (!json["pid"].is_number_integer() || !json["start_time_ms"].is_number_integer() ||
        !json["acquired_at_ms"].is_number_integer() || !json["generation"].is_number_integer()) {
        return fail("pid/start_time_ms/acquired_at_ms/generation 都必须是整数");
    }
    if (!json["instance_token"].is_string()) {
        return fail("instance_token 必须是字符串");
    }
    AccountLockRecord record;
    record.pid = static_cast<unsigned long>(json["pid"].get<std::int64_t>());
    record.start_time_ms = json["start_time_ms"].get<std::int64_t>();
    record.acquired_at_ms = json["acquired_at_ms"].get<std::int64_t>();
    record.generation = json["generation"].get<int>();
    record.instance_token = json["instance_token"].get<std::string>();
    if (record.pid == 0) return fail("pid 不能是 0");
    return record;
}

AccountLock::AcquireResult AccountLock::TryAcquire(const std::filesystem::path& lock_file,
                                                   const AccountLockRecord& self,
                                                   const AliveChecker& alive, AccountLock* out) {
    AcquireResult result;
    if (out == nullptr) {
        result.status = AcquireResult::Status::IoError;
        result.detail = "out 指针为空";
        return result;
    }
    out->Release();

    std::string read_error;
    const auto existing = ReadLockFile(lock_file, &read_error);
    if (existing.has_value()) {
        const AccountLockRecord& holder = *existing;
        // 同一实例重入(同一 ChannelManager 重启账号时撞上自己的旧锁):
        // pid、启动时刻、实例令牌都对上才可续。仅 pid 相同不够——同进程
        // 两只 manager(测试/嵌入式)互不相认。
        const bool same_instance = holder.pid == self.pid &&
                                   holder.start_time_ms == self.start_time_ms &&
                                   holder.instance_token == self.instance_token;
        if (!same_instance) {
            const AliveChecker checker = alive ? alive : DefaultAliveChecker();
            if (checker(holder.pid)) {
                result.status = AcquireResult::Status::RefusedAliveHolder;
                result.holder = holder;
                result.detail = "账号正被 pid " + std::to_string(holder.pid) + " 持有";
                return result;
            }
            // 假死锁:核过进程存活,确实不在了,才清(configuration.md §11)。
            std::error_code ec;
            std::filesystem::remove(lock_file, ec);
            if (ec) {
                result.status = AcquireResult::Status::IoError;
                result.detail = "清假死锁 " + platform::PathToUtf8(lock_file) + " 失败: " + ec.message();
                return result;
            }
        }
    } else if (!read_error.empty()) {
        // 锁文件在但读不懂:不敢删,明报(configuration.md §11"不可见锁便
        // 直接删"的反面——看不懂就更不能删)。
        result.status = AcquireResult::Status::RefusedBrokenLock;
        result.detail = read_error + "(锁文件: " + platform::PathToUtf8(lock_file) + ")";
        return result;
    }

    if (!WriteLockFile(lock_file, self)) {
        result.status = AcquireResult::Status::IoError;
        result.detail = "写锁文件 " + platform::PathToUtf8(lock_file) + " 失败";
        return result;
    }
    out->lock_file_ = lock_file;
    result.status = AcquireResult::Status::Acquired;
    return result;
}

AccountLock::AccountLock(AccountLock&& other) noexcept : lock_file_(std::move(other.lock_file_)) {
    other.lock_file_.clear();
}

AccountLock& AccountLock::operator=(AccountLock&& other) noexcept {
    if (this != &other) {
        Release();
        lock_file_ = std::move(other.lock_file_);
        other.lock_file_.clear();
    }
    return *this;
}

AccountLock::~AccountLock() { Release(); }

void AccountLock::Release() {
    if (lock_file_.empty()) return;
    std::error_code ec;
    std::filesystem::remove(lock_file_, ec);
    // 删失败只剩日志可打;不重试不抛。锁文件残留会在下次 TryAcquire 走
    // 假死锁判定(核 pid)收口。
    lock_file_.clear();
}

}  // namespace lubancode::channel
