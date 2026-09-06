// workspace 目录账本的实现(账本制单子 §一)。键/门牌/重建的规矩见
// index.hpp 头注释与 docs/development/workspace-storage-v2/P0-0-contracts.md
// §一;identity 的归一与 seed 机械复用 identity.cpp,本文件不另造尺。
#include "workspace/index.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "hooks/hash.hpp"
#include "platform/atomic_write.hpp"  // 统一原子写(workspace.json 同款)
#include "platform/paths.hpp"
#include "workspace/storage_contracts.hpp"

namespace lubancode::workspace::index {
namespace {

namespace fs = std::filesystem;
using platform::PathToUtf8;
using platform::Utf8ToPath;

// slug 上限(字节),接哈希后门牌 ≤ 帽+9,离单段名 128 帽都有余量。
// 两平台分档:
//   - POSIX 80:文件系统无 260 之虞,门牌漂亮优先;
//   - Windows 40:门牌 ≤ 49——MAX_PATH 的账要算清:文件 ≤259、目录 ≤247
//     (留尾斜杠一格),session 巢最深一段(sessions/<21>/artifacts/sha256/
//     <2>/<64hex>.tmp-N)还要吃 ~126 字符,再扣 home 根深度。89 字节门牌
//     在深 home(CI 临时根、长用户名)下必爆顶——账本制批 Windows 五红
//     共根于此(2026-09 CI run 34046281141)。
// 门牌只是装饰:账本在,旧房照住(查账走账本,不重算门牌);纯函数恒定,
// 同版内重算即回,新房不裂。
constexpr std::size_t kSlugMaxBytes =
#ifdef _WIN32
    40;
#else
    80;
#endif

std::string ReadTextFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return {};
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

bool IsHexLower(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

// 门牌的哈希段:workspace_key 尾 16 hex 的前 8 位。workspace_key 由
// ComputeWorkspaceKeyFromSeed 铸成 <safe>-<first16(SHA256(seed))>,尾 16
// 就是 seed 哈希的前 16——取其前 8 与"seed 的 SHA-256 前 8 位"同一物,
// 不必在此重算 seed(marker 的声明 id 也不必倒找)。
std::string HashSegmentOf(const WorkspaceIdentity& identity) {
    const std::string& key = identity.workspace_key;
    constexpr std::size_t kTailHex = 16;
    if (key.size() > kTailHex + 1 && key[key.size() - (kTailHex + 1)] == '-') {
        bool hex = true;
        for (std::size_t i = key.size() - kTailHex; i < key.size(); ++i) {
            if (!IsHexLower(key[i])) {
                hex = false;
                break;
            }
        }
        if (hex) {
            return key.substr(key.size() - kTailHex, 8);
        }
    }
    // 手造身份不合形状:对整 key 再哈希兜底,门牌仍确定。
    return hooks::Sha256Hex(key).substr(0, 8);
}

// slug 源路径文本:绝对 + lexically_normal + 正斜杠 + 去尾斜杠,大小写
// 保留(Windows 折叠只发生在账本键/seed 那一侧;门牌要漂亮,原拼写留着)。
std::string PrettyPathText(const fs::path& path) {
    std::error_code ec;
    fs::path absolute = fs::absolute(path, ec);
    if (ec) {
        absolute = path;
    }
    std::string text = PathToUtf8(absolute.lexically_normal());
    for (char& c : text) {
        if (c == '\\') {
            c = '/';
        }
    }
    if (text.size() > 1 && text.back() == '/') {
        text.pop_back();
    }
    return text;
}

bool IsWindowsReservedName(std::string_view name) {
    // 经典保留名集(ASCII,大小写不敏感);门牌带哈希尾实际撞不上,此处
    // 纯防御——撞上就前缀 '_' 换个活法。
    static const char* kReserved[] = {
        "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4",
        "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3",
        "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
    };
    if (name.empty() || name.size() > 4) {
        return false;
    }
    std::string upper;
    upper.reserve(name.size());
    for (const char c : name) {
        upper.push_back(c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
    }
    for (const char* reserved : kReserved) {
        if (upper == reserved) {
            return true;
        }
    }
    return false;
}

bool WriteIndexAtomic(const fs::path& workspaces_root, const WorkspaceIndexMap& entries) {
    nlohmann::json rooms = nlohmann::json::object();
    for (const auto& [key, entry] : entries) {
        rooms[key] = nlohmann::json{
            {"dir", entry.dir},
            {"created", entry.created_at_ms},
        };
    }
    const nlohmann::json out = nlohmann::json{
        {"schema", std::string(contracts::kWorkspaceIndexSchemaName)},
        {"version", contracts::kWorkspaceIndexSchemaVersion},
        {"workspaces", std::move(rooms)},
    };
    return platform::AtomicWriteFile(IndexPath(workspaces_root), out.dump()).has_value();
}

}  // namespace

fs::path IndexPath(const fs::path& workspaces_root) {
    return workspaces_root / "index.json";
}

WorkspaceIndexRead ReadWorkspaceIndex(const fs::path& workspaces_root) {
    WorkspaceIndexRead read;
    std::error_code ec;
    const fs::path path = IndexPath(workspaces_root);
    if (!fs::exists(path, ec) || ec) {
        read.status = WorkspaceIndexRead::Status::Missing;
        return read;
    }
    const auto json = nlohmann::json::parse(ReadTextFile(path), nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        read.status = WorkspaceIndexRead::Status::Corrupt;
        return read;
    }
    // 超版/异 schema:整份弃读走重建(账本是可重建缓存,不是真账;各房
    // manifest 自描述才是)。不猜、不降级读。
    const auto schema = json.find("schema");
    const auto version = json.find("version");
    if (schema == json.end() || !schema->is_string() ||
        schema->get<std::string>() != contracts::kWorkspaceIndexSchemaName ||
        version == json.end() || !version->is_number_integer() ||
        version->get<int>() > contracts::kWorkspaceIndexSchemaVersion) {
        read.status = WorkspaceIndexRead::Status::Corrupt;
        return read;
    }
    const auto rooms = json.find("workspaces");
    if (rooms == json.end() || !rooms->is_object()) {
        read.status = WorkspaceIndexRead::Status::Corrupt;
        return read;
    }
    for (auto it = rooms->begin(); it != rooms->end(); ++it) {
        if (!it->is_object()) {
            continue;  // 单条坏不殃及整本
        }
        WorkspaceIndexEntry entry;
        const auto dir = it->find("dir");
        if (dir == it->end() || !dir->is_string() || dir->get<std::string>().empty()) {
            continue;
        }
        entry.dir = dir->get<std::string>();
        const auto created = it->find("created");
        if (created != it->end() && created->is_number_integer()) {
            entry.created_at_ms = created->get<std::int64_t>();
        }
        read.entries[it.key()] = std::move(entry);
    }
    read.status = WorkspaceIndexRead::Status::Ok;
    return read;
}

std::vector<WorkspaceRoom> ScanRooms(const fs::path& workspaces_root) {
    std::vector<WorkspaceRoom> rooms;
    std::error_code ec;
    if (!fs::is_directory(workspaces_root, ec) || ec) {
        return rooms;
    }
    std::vector<std::string> dirs;
    for (const auto& entry : fs::directory_iterator(workspaces_root, ec)) {
        std::error_code dir_ec;
        if (!entry.is_directory(dir_ec) || dir_ec) {
            continue;  // index.json 等文件不进
        }
        dirs.push_back(PathToUtf8(entry.path().filename()));
    }
    std::sort(dirs.begin(), dirs.end());
    for (const std::string& dir : dirs) {
        const ManifestRead read = ReadWorkspaceManifest(workspaces_root / Utf8ToPath(dir));
        if (read.status != ManifestRead::Status::Ok) {
            continue;  // 坏房/外来目录:不进账,留给 doctor 对账
        }
        WorkspaceRoom room;
        room.dir = dir;
        room.manifest = std::move(read.manifest);
        rooms.push_back(std::move(room));
    }
    return rooms;
}

bool RebuildWorkspaceIndex(const fs::path& workspaces_root) {
    WorkspaceIndexMap entries;
    for (const WorkspaceRoom& room : ScanRooms(workspaces_root)) {
        // 账本键 = identity_root 归一(manifest 存的就是开房时归一过的
        // 文本,再过一遍机械保险)。同键两房按目录名排序先到先得,下次
        // 开张/重建自愈。
        if (room.manifest.identity_root.empty()) {
            continue;
        }
        const std::string key =
            NormalizeIdentityPathText(Utf8ToPath(room.manifest.identity_root));
        if (key.empty() || entries.contains(key)) {
            continue;
        }
        WorkspaceIndexEntry entry;
        entry.dir = room.dir;
        entry.created_at_ms = room.manifest.created_at_ms;
        entries.emplace(key, std::move(entry));
    }
    return WriteIndexAtomic(workspaces_root, entries);
}

std::optional<std::string> LookupWorkspaceDir(const fs::path& workspaces_root,
                                              const std::string& canonical_path_key) {
    if (canonical_path_key.empty()) {
        return std::nullopt;
    }
    WorkspaceIndexRead read = ReadWorkspaceIndex(workspaces_root);
    if (read.status != WorkspaceIndexRead::Status::Ok) {
        // 账本缺/坏:扫各房 workspace.json 自描述重建,再查一次。
        RebuildWorkspaceIndex(workspaces_root);
        read = ReadWorkspaceIndex(workspaces_root);
        if (read.status != WorkspaceIndexRead::Status::Ok) {
            return std::nullopt;  // 重建也写不进(只读盘?);按 miss 走,
                                  // 门牌确定性保底,房不裂。
        }
    }
    const auto it = read.entries.find(canonical_path_key);
    if (it == read.entries.end()) {
        return std::nullopt;
    }
    return it->second.dir;
}

bool RecordWorkspaceEntry(const fs::path& workspaces_root, const std::string& canonical_path_key,
                          const std::string& dir_name, std::int64_t created_at_ms) {
    if (canonical_path_key.empty() || dir_name.empty()) {
        return false;
    }
    WorkspaceIndexRead read = ReadWorkspaceIndex(workspaces_root);
    if (read.status != WorkspaceIndexRead::Status::Ok) {
        RebuildWorkspaceIndex(workspaces_root);
        read = ReadWorkspaceIndex(workspaces_root);
        if (read.status != WorkspaceIndexRead::Status::Ok) {
            read.entries.clear();  // 空/坏账起头:只记这一笔也写得出
            read.status = WorkspaceIndexRead::Status::Ok;
        }
    }
    // 读-改-写:并发记账后写挤掉前写,被挤的房靠门牌确定性 + 下次开张
    // 重记自愈(单子 §一边界 3),不另加锁。
    WorkspaceIndexEntry entry;
    entry.dir = dir_name;
    entry.created_at_ms = created_at_ms;
    read.entries[canonical_path_key] = std::move(entry);
    return WriteIndexAtomic(workspaces_root, read.entries);
}

std::optional<fs::path> ResolveDirByWorkspaceKey(const fs::path& workspaces_root,
                                                 const std::string& workspace_key) {
    if (workspace_key.empty()) {
        return std::nullopt;
    }
    for (const WorkspaceRoom& room : ScanRooms(workspaces_root)) {
        if (room.manifest.workspace_key == workspace_key) {
            return workspaces_root / Utf8ToPath(room.dir);
        }
    }
    return std::nullopt;
}

std::string CanonicalIndexKey(const WorkspaceIdentity& identity) {
    return NormalizeIdentityPathText(identity.identity_root);
}

std::string PathSlug(std::string_view path_text) {
    std::string out;
    out.reserve(path_text.size() < kSlugMaxBytes ? path_text.size() + 8 : kSlugMaxBytes + 8);
    for (const unsigned char byte : path_text) {
        if (out.size() >= kSlugMaxBytes) {
            break;
        }
        if (byte >= 0x80) {
            out.push_back(static_cast<char>(byte));  // 中文/Unicode 原样(UTF-8 透传)
        } else if ((byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'Z') ||
                   (byte >= 'a' && byte <= 'z') || byte == '_' || byte == '-') {
            out.push_back(static_cast<char>(byte));
        } else if (byte == '/' || byte == '\\' || byte == ':' || byte == '.' || byte == ' ') {
            // 分隔符与盘符冒号折 '-':"D:" + "/" → "D--"(样例口径)。
            out.push_back('-');
        } else if (byte == '*' || byte == '?' || byte == '"' || byte == '<' || byte == '>' ||
                   byte == '|' || byte < 0x20) {
            out.push_back('_');  // Windows 非法字符与控制符
        } else {
            out.push_back('-');  // 其余 ASCII 标点(;&%#= 一类)折 '-'
        }
    }
    // 截断可能落在多字节序列中间:残码点(头在尾不在)整段剥掉,不造
    // 非法 UTF-8 文件名。
    if (out.size() > kSlugMaxBytes) {
        out.resize(kSlugMaxBytes);
    }
    if (!out.empty()) {
        std::size_t lead_pos = out.size();
        std::size_t steps = 0;
        while (lead_pos > 0 && steps < 3 &&
               (static_cast<unsigned char>(out[lead_pos - 1]) & 0xC0) == 0x80) {
            --lead_pos;  // 挪过尾部 continuation 字节(一个码点最多 3 个)
            ++steps;
        }
        if (lead_pos > 0) {
            const unsigned char lead = static_cast<unsigned char>(out[lead_pos - 1]);
            std::size_t expected = 1;
            if ((lead & 0xE0) == 0xC0) {
                expected = 2;
            } else if ((lead & 0xF0) == 0xE0) {
                expected = 3;
            } else if ((lead & 0xF8) == 0xF0) {
                expected = 4;
            }
            if (out.size() - (lead_pos - 1) < expected) {
                out.resize(lead_pos - 1);  // 尾上的残段连头一起剥
            }
        } else {
            out.clear();  // 尾上全是 continuation(输入本就烂):剥净
        }
    }
    // 剥尾点尾空格(变换后不该出现,防御 Windows 剥尾规矩)。
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) {
        out.pop_back();
    }
    if (out.empty()) {
        out = "project";
    }
    if (IsWindowsReservedName(out)) {
        out.insert(out.begin(), '_');
    }
    return out;
}

std::string MakeWorkspaceDirName(const WorkspaceIdentity& identity) {
    // slug 源:git/config/cwd 取 project_root(路径漂亮,样例 "D--MinerU-
    // 2604-10547v2" 口径);marker 取声明 id(display_name)——同 id 两处
    // 目录要生成同一块门牌,并账语义才不裂房。
    std::string slug_source;
    if (identity.identity_kind == contracts::kIdentityKindExplicitMarker &&
        !identity.display_name.empty()) {
        slug_source = identity.display_name;
    } else {
        const fs::path& root = identity.project_root.empty() ? identity.checkout_root
                                                             : identity.project_root;
        slug_source = PrettyPathText(root);
    }
    std::string name = PathSlug(slug_source) + "-" + HashSegmentOf(identity);
    // 门牌整名的保留名/剥尾双保险(slug 已各查过;哈希尾在,撞不上)。
    while (!name.empty() && (name.back() == '.' || name.back() == ' ')) {
        name.pop_back();
    }
    if (IsWindowsReservedName(name)) {
        name.insert(name.begin(), '_');
    }
    return name;
}

}  // namespace lubancode::workspace::index
