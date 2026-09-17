# Gateway 服务安装与常驻运维手册(V4 runbook)

[文档首页](../../README.md) · [架构首页](../README.md) · [Gateway 首页](README.md) · [冻结合同](contracts.md)

本页是总装计划(见对应设计单)§十 V4 批的服务运维手册:三平台安装/启停、配置与凭据换代、健康探针、坏账与死信检查、备份与升级回滚、以及**分平台如实列出的未验边界**。

---

## 1. 命令族与语义

```text
lubancode gateway install    [--profile <名>] [--gateway-root <路径>]
lubancode gateway uninstall  [--profile <名>]
lubancode gateway start      [--profile <名>]
lubancode gateway restart    [--profile <名>]
lubancode gateway stop       [--profile <名>]        # 既有命令,语义见 §2
lubancode gateway doctor     [--profile <名>] [--json] [--wait-ready <秒>] [--ack-safe-mode]
lubancode gateway logs       [--profile <名>] [--tail <行>]
```

- **install 只注册,不 start**。注册前校验 profile 配置可装载(`gateway.json` 坏 → 拒装,退 3);凭据/配置体检项当场明列(Warn 以上打印,不自动修——凭据向导归 `lubancode channel setup`)。
- **start/restart 走服务管理器**(schtasks / systemctl / launchctl),不裸 spawn;未 install 明错 `service.not_installed`,不养暗 daemon。手动前台跑仍是 `gateway run`。
- exe 路径、工作目录、参数(含 `--gateway-root` 绝对路径)全部钉死在服务单元里;服务起来不依赖用户环境变量。
- 安装记录落 `<profile>/service/install.json`(exe 路径 + lubancode 版本 + 单元落位),doctor 拿它做升级回滚对账。

## 2. 停止语义(与 supervisor 统一)

所有停止路径先走**文件控制面**:`gateway stop` 投 `control/stop.json`(带目标 boot_id)→ Gateway 按 V0/V1 收口序 drain(StopAccepting → 摘 wake → Close(宽限))→ 锁释放。CLI 不越权代杀;超时如实报 `gateway.shutdown_timeout` 退 4,留给 supervisor 或人工。

- `uninstall` 与 `restart` 的停止段都走同一条文件控制面;没停干净不摘服务、不再拉起(如实报,先人工处置)。
- systemd 的 `systemctl --user stop` 发 SIGTERM,GatewayProcess 的信号面同样进 graceful drain——但 CLI 编排统一走文件面,不各养一套。
- 关机宽限到期未收净的 work **如实入账**:boot-history 的 shutdown 行带 `uncollected_work`(occurrence id 清单),不是 `cancelled`。重启后泵的恢复扫描(先于新派发)按在飞裁决(重派同一 occurrence 或 needs_review)。同步泵(V1/V2)关机时无在飞,清单恒空;异步泵接上后由泵侧填充。

## 3. 三平台安装形态

| | Windows | Linux | macOS |
|---|---|---|---|
| 机制 | 计划任务(schtasks XML,Task Schema 1.3) | systemd user 单元 | launchd LaunchAgent |
| 名字 | `LubanCode Gateway (<profile>)` | `lubancode-gateway-<profile>.service` | `ai.lubancode.gateway.<profile>` |
| 落位 | 注册进任务库;XML 存档在 `<profile>/service/gateway-task.xml` | `~/.config/systemd/user/<unit>` | `~/Library/LaunchAgents/<label>.plist` |
| 自启时机 | 登录触发,延迟 30s | `enable` + `WantedBy=default.target` | `RunAtLoad`(登录/`launchctl load`) |
| 失败重启 | `RestartOnFailure`:间隔 60s、限 3 次 | `Restart=on-failure` + `RestartPreventExitStatus=3` | `KeepAlive={Crashed}`(仅崩溃拉起) |
| 坏配置(退出码 3)防风暴 | 无法按退出码豁免——靠限次(最多重试 3 次);Gateway 自身坏配置不占锁、稳定退 3 | 按退出码豁免,完全不重拉 | 干净退出不拉(含退出码 3) |
| 运行时限 | `ExecutionTimeLimit=PT0S`(不限时) | 无(systemd 默认) | 无 |
| stdout/stderr | cmd 重定向到 `<profile>/logs/service.log` | journal:`journalctl --user -u <unit>` | `StandardOutPath`/`StandardErrorPath` 落 `<profile>/logs/` |
| 手动查 | `schtasks /Query /TN "..."` | `systemctl --user status <unit>` | `launchctl list <label>` |

平台差异如实:**Windows 无按退出码豁免的重启策略**,坏配置下最多被拉 3 次(每次间隔 1 分钟,之后停);systemd/launchd 侧天然豁免。这是 schtasks 能力边界,不是实现选择。

**Linux 注销存活**:systemd user 单元默认随用户会话结束被停。要"关终端、注销后仍跑",需开 linger:

```bash
loginctl enable-linger "$USER"
```

install 不自动开 linger(改系统状态须用户知情);doctor 的 `service.manager_unavailable`/`process.not_running` 面配合 `loginctl show-user "$USER" -p Linger` 自查。

**macOS**:LaunchAgents 目录在登录时自动装载;`launchctl load` 是手动 start。`KeepAlive={Crashed}` 只兜崩溃,干净退出后要 `gateway start` 重拉。

## 4. doctor:体检清单与退出码

`gateway doctor` 一项一码(冻结表见 contracts.md §14):

- 服务注册状态(`service.registered`/`service.not_registered`/`service.manager_unavailable`)
- 锁/实例活态(`process.*`,复用 status 探针)
- 配置可装载(`config.ok`/`config.missing`/`config.invalid`)
- 安装记录对账(`install.ok`/`install.exe_missing`/`install.version_mismatch`)
- 凭据可读,不显值(`credentials.<渠道>.<账号>.ok`/`.resolve_failed`/`.insecure_source`);渠道激活闸其余三闸(trust/lock/执行载体)归 V3 总装,doctor 只跑配置/凭据两闸,不下"渠道能起"的结论(`activation.deferred_to_v3`)
- 磁盘可写(`disk.writable`/`disk.read_only`,即写即删探针,不建目录)
- 坏账(`ledger.needs_review` 计数)、在飞(`ledger.in_flight`;进程没跑时有在飞 = 重启待 reconcile,Warn)、坏行(`ledger.skipped_lines` 语义并入 needs_review/skip 计数)
- 死信(`dead_letter.outbox_flagged` 本地投递 flagged;`dead_letter.channel` 渠道死信)
- SafeMode(`safe_mode.on/off`,连击/阈值)与最近一次关机(`boot.last_shutdown_clean/unclean`,unclean 行带未收净清单)

**退出码(稳定)**:`0` 全绿(Info/Ok);`1` 有 Warn;`2` 有 Fail。`--json` 出机器可读清单。外部监控按退出码告警,不要解析人话。

零副作用边界:doctor(不带 `--ack-safe-mode`)零写盘零建目录。

### 健康探针

```bash
lubancode gateway doctor --wait-ready 30
```

install 后验证与外部监控用:轮询至"锁被活进程持有 + control 报 running + health ok + 非 SafeMode"。ready 退 0(随后照常出体检清单);超时如实退 1 并报 `gateway.not_ready`,不假 ready。**进程 running 不等于 ready**——SafeMode 下业务面暂停,不算 ready。

### SafeMode 显式确认

连续非干净关机达阈值(默认 3)进 SafeMode(业务面暂停,控制面照起)。排查后:

```bash
lubancode gateway doctor --ack-safe-mode
```

boot-history 落一行 `type=ack`:连击清零,效力同干净关机——但账上保留的是人工确认事实,不是伪造的 shutdown。没有 ack 口之前,只有一场真实干净关机能清连击。

## 5. 配置/凭据换代

- `gateway.json`(profile 配置)手改后 **restart** 生效;坏配置下 Gateway 稳定退 3、systemd/launchd 不重拉(Windows 最多再拉 3 次),不会重启风暴。
- 渠道凭据换代走 `lubancode channel setup`(向导;本 runbook 不管);换代后 `gateway doctor` 验 `credentials.*` 各项——旧 secret 文件在换代窗口内由 CredentialStore 受管回收,不在本面重复。
- 账号失效(凭据撤销/过期):doctor 报 `credentials.<...>.resolve_failed`(稳定码如 `secret_file_insecure`);渠道面投递终态失败进 `dead_letter.channel`。处置:向导重配 → `gateway restart`。

## 6. 备份与升级回滚

**备份**:整棵 `<状态根>/gateway/`(profiles/automation/delivery/boot-history)是运行状态账,冷备就够——停机后整目录拷走。会话正文在 workspaces 树(V3 账),备份范围按需另定。

**升级**:

```bash
lubancode gateway stop                 # 文件面干净停
# 替换 exe(安装位路径不变,服务单元不用动)
lubancode gateway install              # 刷新单元与 install.json(钉新版本)
lubancode gateway start
lubancode gateway doctor --wait-ready 30
```

服务单元钉的是 exe **绝对路径**:若新版本装在新路径,必须重跑 `gateway install`(doctor 的 `install.version_mismatch`/`install.exe_missing` 会提示)。

**手动回滚**(版本回退):

```bash
lubancode gateway stop
# 把旧版 exe 放回 install.json 记录的路径(或改放后重跑 install)
lubancode gateway install              # 让单元与记录对上
lubancode gateway start
lubancode gateway doctor
```

账面向前兼容:领域账(automation/outbox)行类型纯追加,未知 type 读取侧跳过——回滚到认得旧行的版本安全;新行落在账上时旧版本会跳过(不崩),对应新任务在旧版本下不跑,升回来接着跑。

## 7. logs

`gateway logs [--tail N]` 打 boot-history 与 gateway.log 的尾 N 行,并指服务 stdout/stderr 落位(Windows/macOS:`<profile>/logs/service.log`;Linux:journalctl)。不做日志聚合,不尾随(要 follow 用平台工具:`Get-Content -Wait` / `journalctl -f` / `tail -f`)。

## 8. 未验边界(分平台如实)

CI 验的是**生成物文本与命令面**(三平台单元文本断言、注入 runner 的 argv 形状、doctor 码表);**真实服务注册与常驻行为三平台真机未验**:

| 平台 | 已验(CI) | 未验(真机) |
|---|---|---|
| Windows | XML 生成物、schtasks argv 形状、doctor 码表 | `install` 真注册、`start` 真拉起、登录触发生效、关机/注销行为、失败重启限次、24h 常驻 |
| Linux | unit 生成物、systemctl argv 形状、doctor 码表 | 真机 `daemon-reload`/`enable`/`start`、注销后 linger 存活、`RestartPreventExitStatus=3` 真生效 |
| macOS | plist 生成物、launchctl argv 形状、doctor 码表 | 真机 `load`/`kickstart`、登录自启、`KeepAlive={Crashed}` 行为 |

单子放行门(首个平台关闭终端、注销或重启后的实测记录;24 小时常驻)待 Windows 真机验收后回填。先交付 Windows 实测路径,不宣称三平台完成。
