// workspace 目录账本(workspaces/index.json)——路径为键、漂亮门牌为值
// 的索引查找(账本制单子《workspace目录账本制命名与索引》)。
//
// 账本制三步(单子 §一"找门三步"):
//   1. canonical 路径 → 查账本 → 门牌 → 开房(O(1));
//   2. miss(新项目)→ 生成门牌 → 开房(workspace.json 照写,自描述保留)
//      → 记账(原子写);
//   3. 账本缺/坏 → 扫各房 workspace.json 自描述重建(房在账可回;并发开房
//      挤掉账目也靠这手自愈)。
//
// 键与门牌:
//   - 账本键 = NormalizeIdentityPathText(identity.identity_root)——与算
//     workspace_key 的 seed 归一是同一把尺(identity.cpp 现行机械,不另造)。
//     git 取 common git dir,主树与 linked worktree 同键同房;
//   - 门牌 = <路径slug(POSIX ≤80 / Windows ≤40 字节)>-<seed SHA-256 前
//     8 hex>。slug 保大小写、中文/Unicode 原样;哈希段保唯一(不同 seed
//     必不同门牌)。Windows 档把门牌压到 ≤49:MAX_PATH(文件 259/目录
//     247)要给 session/artifacts 深巢(~126 字符)与 home 根深度留预算,
//     89 字节门牌在深 home 下会让巢底文件全数打不开(2026-09 Windows 五红
//     共根)。门牌只是装饰,身份仍是 workspace_key(manifest/session.json
//     里的那枚)。
//
// 硬规矩:
//   - 写入走 platform::AtomicWriteFile(workspace.json 并发原子写同款);
//   - 账本是可重建缓存:读侧见坏 JSON/超版整份弃读,扫房重建覆盖,不猜;
//   - 门牌生成是纯函数:同 identity 恒同门牌——账本丢了重算即回,并发
//     miss 各自生成的门牌也一致,不裂房;
//   - 无历史包袱(工头令):不写任何老目录名兼容/改名逻辑。
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/identity.hpp"
#include "workspace/manifest.hpp"  // WorkspaceManifest(房的自描述)

namespace lubancode::workspace::index {

// 账本文件名(<workspaces_root>/index.json)。
std::filesystem::path IndexPath(const std::filesystem::path& workspaces_root);

struct WorkspaceIndexEntry {
    std::string dir;            // 漂亮门牌(目录名,单段)
    std::int64_t created_at_ms = 0;
};

using WorkspaceIndexMap = std::map<std::string, WorkspaceIndexEntry>;

struct WorkspaceIndexRead {
    enum class Status { Ok, Missing, Corrupt };
    Status status = Status::Missing;
    WorkspaceIndexMap entries;  // status==Ok 时有效
};

// 读账本。missing = 文件不存在;corrupt = 坏 JSON/非对象/超版(整份弃读,
// 调用方走重建,不猜)。
WorkspaceIndexRead ReadWorkspaceIndex(const std::filesystem::path& workspaces_root);

// 查账(找门三步的口):账本缺/坏先扫房重建再查一次;仍 miss 给 nullopt。
// 命中只回门牌名;门牌指的房在不在盘上由调用方验(不在按 miss 重开门,
// 门牌确定性保证开回原名)。
std::optional<std::string> LookupWorkspaceDir(const std::filesystem::path& workspaces_root,
                                              const std::string& canonical_path_key);

// 记账:读-改-写,原子替换整份。账坏先重建再并笔。失败回 false(账本是
// 缓存,房自描述在盘上,丢了靠重建/下次开张自愈——调用方可忽略)。
bool RecordWorkspaceEntry(const std::filesystem::path& workspaces_root,
                          const std::string& canonical_path_key, const std::string& dir_name,
                          std::int64_t created_at_ms);

// 一间房的自描述(扫房产物):门牌 + manifest。
struct WorkspaceRoom {
    std::string dir;
    WorkspaceManifest manifest;
};

// 扫 workspaces_root 下各房,读 workspace.json。只收 manifest 可读的房
// (坏房/外来目录不进账);目录名排序,结果确定。
std::vector<WorkspaceRoom> ScanRooms(const std::filesystem::path& workspaces_root);

// 扫各房 workspace.json 自描述重建账本并原子写回。每房按 manifest 的
// identity_root 归一出账本键;同键两房(异常现场)按目录名排序先到先得,
// 下次开张自愈。workspaces_root 不存在给空账(首开前的常态)。
bool RebuildWorkspaceIndex(const std::filesystem::path& workspaces_root);

// 按 workspace_key(身份串,非门牌)反查房门:扫各房 manifest 匹配。消费
// 方(session 索引/搬删命令/CLI)手里只有 key 时走这里;目录名排序取首个,
// 结果确定。找不到给 nullopt。
std::optional<std::filesystem::path> ResolveDirByWorkspaceKey(
    const std::filesystem::path& workspaces_root, const std::string& workspace_key);

// ---------------------------------------------------------------------------
// 门牌生成(纯函数;测试与诊断直接吃)
// ---------------------------------------------------------------------------

// 门牌 = <slug>-<seed哈希前8>。slug 源:git/config/cwd 取 project_root 路径
// (保大小写);marker 取声明 id(display_name)——同 id 两处目录要得出同
// 一块门牌,团队并账语义才成立。哈希段从 identity.workspace_key 的尾 16
// hex 里取前 8(与 seed 的 SHA-256 前 8 位同一物),不合形状(手造身份)
// 兜底对整 key 再哈希,门牌仍确定。
std::string MakeWorkspaceDirName(const WorkspaceIdentity& identity);

// slug 变换:分隔符 \ / 与 : . 空格 → '-';非法字符 * ? " < > | 与控制符
// → '_';其余 ASCII 标点折 '-';[A-Za-z0-9_-] 与多字节(中文/Unicode)
// 原样。超平台帽(POSIX 80 / Windows 40 字节)截断(UTF-8 序列边界回退);
// Windows 保留名(CON/PRN/NUL/COM1-9/LPT1-9,大小写不敏感)前缀 '_';
// 剥尾点尾空格;空串回 "project"。
std::string PathSlug(std::string_view path_text);

// 账本键:identity_root 过 identity 的归一机械(正斜杠/去尾斜杠/Windows
// 折小写)。开房路与重建路都从这一枚函数取键,不各算各的。
std::string CanonicalIndexKey(const WorkspaceIdentity& identity);

}  // namespace lubancode::workspace::index
