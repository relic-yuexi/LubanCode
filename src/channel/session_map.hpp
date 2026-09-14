// ChannelSessionMap:渠道会话键 → V3 场的持久映射(QQ 接入单 Q2,§六
// 第二/第四项)。
//
// 账规矩:
//   - 键 = (session_key, workspace_key)。session_key 由路由定
//     ("channel:<ch>:<acct>:<kind>:<conv>");workspace_key 是启动时冻结的
//     工作目录身份——同一会话键在不同工作目录下各开各场,重启换 cwd 不
//     误续别的项目上下文。同一机器人账号被多个工作目录同时占用由账号锁
//     (AccountLock,AddAccount 时已取)挡在前头;这里管的是"锁没挡住的
//     先后场景":A 目录用过 → 停 → B 目录用,B 的 workspace_key 对不上
//     映射,开新场,不续 A 的上下文。
//   - 追加式 JSONL(trajectory::JournalWriter PowerLoss):每行
//     {"t":"mapped","sessionKey":...,"workspaceKey":...,"sessionId":...,
//      "atMs":...}。同一 (键,workspace) 的 resume-as-new 会追加新行,
//     重放取最后一行为准——映射随每场更新(§六第四项)。
//   - 纯账件:不验证 sessionId 是否真有场(恢复路经 V3 resolver 验,
//     验不动的按 needs_review 处置);不持会话生命周期。
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>

#include "trajectory/journal.hpp"

namespace lubancode::channel {

class ChannelSessionMap {
public:
    // 值语义被 mutex 禁:经 Open 工厂就地构造(装配方持 optional/unique_ptr)。
    ChannelSessionMap() = default;
    ~ChannelSessionMap() = default;
    ChannelSessionMap(const ChannelSessionMap&) = delete;
    ChannelSessionMap& operator=(const ChannelSessionMap&) = delete;

    struct OpenResult {
        bool ok = false;
        std::string error;
    };
    // 打开(或新建)账号的映射账。map_file =
    // <state_root>/<channel>/<account>/sessions.jsonl(由调用方拼好递入)。
    static OpenResult Open(ChannelSessionMap* out, const std::filesystem::path& map_file);

    bool broken() const { return broken_; }
    const std::string& last_error() const { return last_error_; }

    // 记映射(同键同 workspace 同 sessionId = 幂等 no-op;变了才追加)。
    // 返回 false = 账写不进(broken;调用方应停推进,不冒充映射已更新)。
    bool Map(const std::string& session_key, const std::string& workspace_key,
             const std::string& session_id, std::int64_t at_ms);

    // 查映射:最后一笔生效的 sessionId;没账目给 nullopt(开新场)。
    std::optional<std::string> Find(const std::string& session_key,
                                    const std::string& workspace_key) const;

    // 观测:映射条数(诊断/测试)。
    std::size_t size() const;

private:
    std::filesystem::path map_file_;
    bool broken_ = false;
    mutable std::string last_error_;
    mutable std::mutex mutex_;
    std::optional<trajectory::JournalWriter> writer_;  // 首笔才开(占位零写盘)
    // (session_key, workspace_key) -> 最新 session_id。mutex 件不落值语义:
    // 工厂开账,装配方持对象。
    std::map<std::pair<std::string, std::string>, std::string> latest_;
};

}  // namespace lubancode::channel
