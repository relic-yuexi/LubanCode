// Soul 会话冻结(System 拼装 Hook/Soul 会话冻结与缓存边界单 P0)。
//
// 单内 §5.1 定案的双状态:
//   configuredSoul —— 持久化默认值(SOUL.md + 配置 soul: 项),供以后新建
//                     会话读取;app 层的 current_soul/current_soul_name
//                     就是它的内存映像,不在本件。
//   sessionSoul    —— 本会话采用的名称、规范化正文、来源/hash、revision
//                     与 locked 状态,即本件的 SessionSoulSnapshot。
//                     锁定边界:空闲且尚未准备首个模型请求时可改;首请求
//                     准备一开始即锁定,此后失败/取消/零输出都不解锁。
//
// 快照正文存 blob:锁定那一刻由 TrajectorySessionLedger::CommitSoulSnapshot
// 把整份快照写进会话目录(WriteSessionSoulSnapshot,原子落盘);resume 恢复
// 只认 blob 里的正文,不能只凭魂名重读磁盘文件(§5.3"恢复材料缺失则报错,
// 不静默换魂")。这不是第二本事件账——是已采用材料的 blob 存储,与 v3 的
// system 正文 blob 同一条纪律。
//
// 本件只放纯数据与纯函数,不认终端、不认轨迹写口;单测直接钉这里。
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace lubancode::runtime {

// 一份会话 Soul 快照。会话存续期由装配层持有(共享指针,后台派工的冻结
// 后端 spawner 也借它读);锁定时经账本提交进会话目录。
struct SessionSoulSnapshot {
    std::string name;       // "default" / "off" / souls/<名字>.md 的具名
    std::string content;    // 采用的正文原文(注入时剥注释);off/空魂为空串
    std::string source;     // 来源说明,如 "config:default" / "resume:<session_id>"
    std::string content_hash;  // 规范化正文的 sha256(NormalizeSoulContent 后)
    std::int64_t revision = 0;  // 草稿修订号,锁定前每次成功改动 +1,锁定后恒定
    bool locked = false;    // 首请求锁定后恒 true;失败/取消/换模型都不解锁

    bool operator==(const SessionSoulSnapshot&) const = default;
};

// 规范化正文:剥提示词注释、统一行尾、去首尾空白。内容是否"未变"以这
// 份为准(§5.2"内容规范化后相同要提示内容未变,不记虚假 pending/revision")。
std::string NormalizeSoulContent(const std::string& content);

// 规范化正文的 sha256(十六进制小写)。
std::string SessionSoulContentHash(const std::string& content);

// 依当前字段重算 content_hash 并递增 revision 的草稿更新口(锁定判断在
// 调用方:锁定后的会话不该走到这里)。content 未规范化变化时不递增
// revision、返回 false——由调用方提示"内容未变"。
bool UpdateSessionSoulDraft(SessionSoulSnapshot& snapshot, std::string name, std::string content,
                            std::string source);

// 快照 <-> JSON(落盘格式;解析一律先 contains 再取值——const json 的
// operator[] 查缺键是 UB,nlohmann 单列禁令)。
nlohmann::json SessionSoulSnapshotToJson(const SessionSoulSnapshot& snapshot);
// 缺字段/类型不对给错误串,不半造快照。
std::expected<SessionSoulSnapshot, std::string> SessionSoulSnapshotFromJson(const nlohmann::json& json);

// ---- 会话目录里的 soul 快照 blob ----
// 文件名固定 soul-snapshot.json,落在会话目录根(v2/v3 场同款)。写法:
// 先写同目录临时文件再原子换名,读回校验 content_hash 与正文一致——
// 材料坏了给错误,resume 侧据此报错,不静默换魂。
std::string SessionSoulSnapshotFileName();
// 写失败给错误串(目录没有写口/盘满);成功覆盖旧快照(以后一次成功
// 保存为准,§5.2)。
std::expected<void, std::string> WriteSessionSoulSnapshot(const std::filesystem::path& session_dir,
                                                          const SessionSoulSnapshot& snapshot);
// 文件不存在给 nullopt(源会话从未锁定过魂);存在但坏了给错误串。
std::expected<std::optional<SessionSoulSnapshot>, std::string> ReadSessionSoulSnapshot(
    const std::filesystem::path& session_dir);

}  // namespace lubancode::runtime
