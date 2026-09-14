// 常驻助理宿主(常驻助理 Web 主界面单 W0/W1):
//
//   `lubancode assistant [--no-open] [--port N] [--profile default]`
//
// 一只前台进程同时装两样(§三分层):
//   - LocalWebServer:随包网页(HTML/JS/CSS,无外部 CDN)+ 本地认证
//     (bootstrap 一次性凭据换 HttpOnly/SameSite=Strict 会话 cookie)+
//     WS 升级门(同源/Host 校验,拒 DNS rebinding 与跨站);
//   - AppServer 控制入口:同一只 app_server::Server(1.3 协议、
//     clientOperationId 幂等、thread/turn/operation 面),助理模式
//     (WorkLifetime::Detached):WS 断线只撤订阅,已受理的回合照跑到
//     终态、落账——关标签/刷新不打断工作;turn/interrupt 是"停止任务",
//     shutdown 是"停止助理",断线不是其中任何一个。
//
// 任务归属与执行路由(W0 冻结合同,见 docs/features/assistant-web/README.md):
//   - 页面立即聊天:只经 AppServer 的 turn/start(一场 thread 同拍一轮,
//     kErrTurnAlreadyRunning 明拒;clientOperationId 同键回原受理,不双跑);
//   - 后台/定时任务:Gateway 调度(AutomationStore→GatewayAutomationPump)。
//     本类留有装配缝(W0 注记):RunGateway→服务提取正由另一工位在做,
//     这里不提取、不另起工作泵;提取并进后在此挂复合泵,页面任务入口
//     (W2)经同一 AutomationStore 控制服务进账,不经聊天线。
//
// 启动与停止(§四):
//   - 默认系统分配端口、只绑 127.0.0.1;指定端口被占准确报错不偷换;
//   - 监听就绪、接口健康之后才开浏览器;打不开保留可复制 URL;
//   - 重复启动同 profile:读锁账→探活→/healthz 对 bootId 验明正身→
//     /control/open 拿新打开页 URL→开其页面后退出。探测失败/锁不明:
//     报错退出,不杀原进程、不复用可疑 URL;
//   - 前台进程随终端退出即停止(第一阶段如实说明);Ctrl+C 走宽限收口
//     (停收新活→打断收口→落账→释放监听与锁)。
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace lubancode::config {
struct ConfigResult;
}

namespace lubancode::app {

struct AssistantCliArgs;

// assistant 子命令的主入口(cli_app 直调;自己装配置、不依赖外层装配)。
// 返回进程退出码:0 干净收口;1 启动/装配失败;2 已有实例(已按要求
// 只开页面);3 锁不明/探测失败。
int RunAssistantMode(const AssistantCliArgs& args);

}  // namespace lubancode::app
