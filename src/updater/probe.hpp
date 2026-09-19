// 更新助手 C++ 化·批三第①单(引擎主链):无副作用健康探针。
//
// 语义唯一真源 scripts/updater.py 的 probe_exe(L574-598):
//   - 隔离数据根跑 `<exe> --version`:LUBANCODE_HOME/LUBANCODE_DATA_HOME
//     指到临时目录(不是版本目录、不是用户根,§七"不碰真实用户数据"),
//     LUBANCODE_LANG=en 钉英文输出,超时 30s(PROBE_TIMEOUT_SECS)超时杀树;
//   - 首行协议:startswith("lubancode ")——rc 非 0 或首行不合即"探针输出
//     不合";expected_version 给了就精确对 "lubancode <版本>",不合即
//     "探针版本不合";
//   - 起不来(spawn 失败/超时)即"探针跑不起来"——三路人话文案与 python
//     逐字同源,直接进事务账(health_probe)与日志。
//
// 口径差(如实声明):
//   - stdout 走 RunProcessWithStdin 的 stdout_bytes 专线捕获(python 读
//     out.stdout)——不用合并管道口,stderr 噪声不污染首行判定;退出码与
//     首行判定的先后、格式照 python;
//   - python %r 的单引号包装在 C++ 侧以裸引号拼出,诊断文案等价。
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace lubancode::updater {

// 探针超时(秒),python PROBE_TIMEOUT_SECS 同款。
inline constexpr int kProbeTimeoutSecs = 30;

// 探针结果:ok=false 时 detail 是人话原因;成功时 detail 是首行
// "lubancode <版本>"(python 的 (ok, detail) 二元组同形)。
struct ProbeOutcome {
    bool ok = false;
    std::string detail;
};

// 跑一次探针。expected_version 给 nullopt 只验"跑得起来、印的是 lubancode
// 头";给了就精确对版本。临时数据根用后即清。
ProbeOutcome ProbeExe(const std::filesystem::path& exe_path,
                      std::optional<std::string_view> expected_version = std::nullopt,
                      int timeout_secs = kProbeTimeoutSecs);

}  // namespace lubancode::updater
