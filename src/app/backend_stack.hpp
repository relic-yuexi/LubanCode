// 会话后端栈(骨架拆解批四收口):按 wire 造真实 client(BuildBackend),
// 外加一只稳定壳(RebuildableBackend)——会话里切 provider 时,Spinner、
// ToolRuntime、模型路由、Agent 都还握着 Backend&;这层把真正的 client 藏
// 起来并按需替换,引用地址不变,下一次请求自然落到新 base_url/wire/key
// 上。切端的绑定规矩(病十一其二):provider 字符串与 backend 引用只在
// HandleProviderCommand 一条路上一起换(Rebuild + rebuild_loop 重建皮),
// 不再有第二处改字符串不动后端的旁路。
//
// 从前的五层请求改写后端(Model/Think/ModelInstructions/SoulOverlay/
// DeferredIndex,backend_stack.hpp:52-158 的老账)已整体退役:那五样会话
// 级请求策略当年为不碰 agent 文件全挂传输层,现在归皮上管道——model/
// effort 走 AgentProfile.request,模型指令/魂/延迟索引是皮上叠层,由
// Agent 拼请求时就地生效(见 agent/agent.hpp 的 AgentProfile 注释);
// /model、/think、/soul 的即时生效由会话层 SyncAgentRequestPolicy 同步。
//
// 这一层只认 api/config 的既有抽象,不 include 交互会话的东西;可直测。
//
// 实现在 backend_stack.cpp(编译边界:头文件只放类形状与函数声明,具体
// client 的依赖都留在 .cpp 一侧)。
//
// SpinnerBackend 原先住这(最外层起停"思考中"转轮);骨架拆解批二把它
// 挪去了 cli/spinner_backend.hpp——UI 件不混传输层。

#pragma once

#include <atomic>
#include <expected>
#include <functional>
#include <memory>
#include <string>

#include "api/backend.hpp"
#include "config/config.hpp"

namespace lubancode::app {

// 按 wire 造对应的后端实现。agent 层只认 Backend 这个抽象接口,不关心
// 背后具体是哪个协议在干活。
std::unique_ptr<lubancode::api::Backend> BuildBackend(const lubancode::config::Config& config);

// 会话里切 provider 时,外层包装器、Agent 和工具都还握着 Backend&。这一层
// 把真正的 client 藏起来并按需替换,引用地址不变;下一次请求自然落到新
// base_url/wire/key 上。
class RebuildableBackend : public lubancode::api::Backend {
public:
    explicit RebuildableBackend(const lubancode::config::Config& config);

    void Rebuild(const lubancode::config::Config& config);

    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request& request,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override;

private:
    std::unique_ptr<lubancode::api::Backend> inner_;
};

}  // namespace lubancode::app
