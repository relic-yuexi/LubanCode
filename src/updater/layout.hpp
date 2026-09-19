// 更新助手 C++ 化·批二第③单:安装根布局层——相对布局、三态探测、
// current.json 指针与 install-state.json(schema 2)的读写。
//
// 语义唯一真源 scripts/updater.py(§六布局):
//   - layout_paths(L156-168):install_root 下的相对布局——versions/、
//     staging/、backups/、updates/ 四目录 + current.json/install-state.json
//     两账 + 根位 EXE + updater/ 受管树;
//   - detect_layout(L192-199):current.json 在 = versioned;根 EXE 或
//     install-state.json 在 = flat(旧平铺);否则 empty;
//   - read_current(L179-189)/activate(L965-972):current.json schema 1,
//     current 必须非空字符串;写侧带 previous/updated_at_utc/transaction。
//     读侧单段名严格口径(拒 '/'、'\\'、'.'、'..')出 src/app/launcher.cpp
//     的 ReadCurrentPointer——启动器与更新器认同一份指针,别各养一套;
//   - write_install_state(L854-878):schema 2 十三字段(installed_at_utc/
//     version/platform/channel/source 七件套/installer/transaction/
//     manifest_provenance/manifest/pending_conflicts);cmd_rollback(L1330-1334)
//     的追加字段 rolled_back_at_utc 读侧保留、写侧仅在置值时落。
//
// 口径差(如实声明):
//   - 读侧统一走 ReadJsonFileTolerant(utf-8-sig 容错,坏 JSON 当无),
//     与 python read_json_file(L171-175)同款;WriteCurrent 比python 多一道
//     单段名校验——读侧既然拒,写侧也不该把读侧必拒的指针写出去,收紧
//     方向,不放松;
//   - 落盘时间戳 UTC ISO8601 秒精度 Z 尾(UtcNowIso8601),与 python
//     utcnow() 同格式;钟可经 seam 注入(测试钉死)。
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace lubancode::updater {

// install-state.json 的 schema 号(版本化布局,updater.py SCHEMA_STATE)。
inline constexpr int kInstallStateSchema = 2;
// current.json 的 schema 号(updater.py SCHEMA_CURRENT)。
inline constexpr int kCurrentSchema = 1;

// 安装根的相对布局(updater.py layout_paths)。路径全部由 root 拼出,
// 目录不预建——建目录是各使用侧的事(锁建 updates/,事务建 updates/ 与
// staging/,激活建 versions/)。
struct LayoutPaths {
    std::filesystem::path root;      // 安装根(启动器所在目录)
    std::filesystem::path versions;  // versions/    不可变整包
    std::filesystem::path current;   // current.json 当前与上次可用指向
    std::filesystem::path state;     // install-state.json 安装来源与事务索引
    std::filesystem::path staging;   // staging/     未激活下载与解包
    std::filesystem::path backups;   // backups/     旧安装与用户修改备份
    std::filesystem::path updates;   // updates/     事务日志与安装锁
    std::filesystem::path exe;       // lubancode(.exe) 根位固定启动器
    std::filesystem::path updater;   // updater/     受管更新助手树
};

// 拼一份安装根布局(root 原样使用,不 abspath——绝对化归调用方)。
LayoutPaths MakeLayoutPaths(const std::filesystem::path& root);

// 布局三态(updater.py detect_layout):
//   Versioned —— current.json 在(版本化布局,一键更新的正身);
//   Flat      —— 旧平铺安装(根 EXE 或 install-state.json 在);
//   Empty     —— 什么都没有(全新安装)。
enum class LayoutKind {
    Empty,
    Flat,
    Versioned,
};

// 探测安装根的布局三态(只读,不建任何东西)。
LayoutKind DetectLayout(const std::filesystem::path& root);

// 容错读 JSON 文件(python read_json_file 同款):utf-8-sig 剥 BOM、
// 坏 JSON/打不开/不存在一律 nullopt。更新器所有账文件的读侧共口。
std::optional<nlohmann::json> ReadJsonFileTolerant(const std::filesystem::path& file);

// UTC ISO8601 秒精度 Z 尾(python utcnow() 的 "%Y-%m-%dT%H:%M:%SZ")。
std::string UtcNowIso8601();

// 钟的 seam 类型:返回 UTC ISO8601 串。空 seam 用真钟。测试注固定钟
// 钉死落盘字节,黄金对拍才可复现。
using UtcNowFn = std::function<std::string()>;

// current.json 读出的指针。
struct CurrentPointer {
    std::string current;                 // 当前版本目录名(单段)
    std::optional<std::string> previous; // 上次可用版本目录名;无则 nullopt
};

// 读 current.json。文件不在/坏 JSON/schema 不对/current 不是合格单段名
// (拒 '/'、'\\'、'.'、'..')一律 nullopt——启动器同款严格口径,坏指针
// 当没有,不猜。previous 只在是非空字符串时给值。
std::optional<CurrentPointer> ReadCurrent(const LayoutPaths& paths);

// 单段目录名校验(启动器 ReadCurrentPointer 口径):非空、无 '/'、无
// '\\'、不是 "." 或 ".."。current.json 的 current 与 previous 都用它。
bool IsValidVersionDirname(std::string_view name);

// 原子写 current.json(schema 1):{schema, current, previous, updated_at_utc,
// transaction}。previous 无则 null(python activate 的指针形状)。current
// 不是合格单段名抛 std::runtime_error(见文件头口径差)。写走
// AtomicWriteFile(ProcessCrashDurability),同文件系统单次替换。
void WriteCurrent(const LayoutPaths& paths, const std::string& current,
                  const std::optional<std::string>& previous, const std::string& transaction,
                  const UtcNowFn& now = {});

// install-state.json(schema 2)的账。字段照 python write_install_state:
// source 七件套 + installer/transaction/manifest 等;rolled_back_at_utc 是
// cmd_rollback 的追加字段,置值才落盘。manifest 是官方清单原样(json),
// 读侧原样保留(所有权基线要用)。
struct InstallState {
    std::string installed_at_utc;
    std::string version;
    std::optional<std::string> platform;
    std::string channel;  // stable / prerelease

    std::optional<std::string> source_repo;
    std::optional<std::int64_t> source_release_id;
    std::optional<std::string> source_release_tag;
    std::optional<std::int64_t> source_asset_id;
    std::optional<std::string> source_asset_name;
    std::string source_asset_digest;          // "sha256:<hex>"
    std::optional<std::string> source_download_url;

    std::string installer;                    // 记账:谁装的(python 写 "updater.py")
    std::string transaction;                  // 提交事务 id
    nlohmann::json manifest;                  // 官方清单(原样)
    std::optional<std::string> rolled_back_at_utc;  // rollback 追加字段
};

// 读 install-state.json。文件不在/坏 JSON/不是 object/schema 不是 2 一律
// nullopt;字段逐个容错(缺了给空值,类型不对给空值),不因个别字段坏
// 整份拒——账是事实记录,能读多少读多少。
std::optional<InstallState> ReadInstallState(const LayoutPaths& paths);

// 原子写 install-state.json(schema 2)。manifest 为 null 时原样落 null
// (调用方负责先填);pending_conflicts 恒落 [](python 写侧从无他值)。
// rolled_back_at_utc 置值才落。installed_at_utc 取 now seam。
void WriteInstallState(const LayoutPaths& paths, const InstallState& state, const UtcNowFn& now = {});

}  // namespace lubancode::updater
