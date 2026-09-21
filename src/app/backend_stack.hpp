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
#include <mutex>
#include <optional>
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
// 线程安全(W2 起):Rebuild 与 send_stream 可从不同线程并发——内芯持
// shared_ptr,send_stream 锁内快照后锁外用,在飞请求持旧内芯跑到完
//(换血只影响下一次请求,不撕在途流)。
//
// HC-08(包装层预算映射与出站能力转发):这层壳原先只 override
// send_stream,其余能力吃 Backend 基类默认——SerializeForDiagnostics 空
// 串、BuildWireMessageMap/PrepareWireRequest 不可得、GetEffectiveOutputLimit
// 回退 Request::max_tokens、Force no-op。真实终端链(Agent 只认 Backend&,
// 握的正是这层壳)于是丢了叶 client 的合同:adapter 预算退内部估算、
// provider 覆盖后的有效上限读不着、收窄写不进最终 wire。修法是逐项窄
// 转发给内芯,壳的职责(稳定地址/换血)一分不加:不是协议适配器,就
// 不掺协议;每口一次快照,锁外调内芯。
class RebuildableBackend : public lubancode::api::Backend {
public:
    explicit RebuildableBackend(const lubancode::config::Config& config);
    // 预装内芯口(HC-08 合同测试用):直接给定现成内芯,生产路走上面的
    // Config 构造。测试往里注能记账的 fake inner,验证转发本身;换血
    //(Rebuild)与锁规矩与生产路同一条,不是旁门。
    explicit RebuildableBackend(std::shared_ptr<lubancode::api::Backend> inner);

    void Rebuild(const lubancode::config::Config& config);

    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request& request,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override;

    // 以下五口(HC-08)窄转发内芯。同步方案:与 send_stream 同一把锁、
    // 同一套快照规矩——const 查询也先在锁内拷 shared_ptr 快照,放锁后再
    // 调内芯;一次转发从头到尾落在同一配置代上,也不持锁跨网络/流式
    // 调用(Rebuild 只换指针,不等在途调用)。跨调用(预算判定在前、
    // 最终发送在后)的代绑定是 FD-05 采用视图提交链的接缝:Backend 接口
    // 面上没有把 prepared 结果递进 send 的把柄,这层不另攒状态去猜。
    // 未装配(快照为空)按各口接口合同返回错误/不可得——即基类默认
    // 形态,不冒充实数。
    std::string SerializeForDiagnostics(const lubancode::api::Request& request) const override;
    lubancode::api::PreparedWireRequest PrepareWireRequest(
        const lubancode::api::Request& request) const override;
    std::optional<lubancode::api::WireMessageMap> BuildWireMessageMap(
        const lubancode::api::Request& request) const override;
    EffectiveOutputLimit GetEffectiveOutputLimit(const lubancode::api::Request& request) const override;
    void ForceMaxOutputTokensOverride(lubancode::api::Request& request, int tokens) const override;

private:
    // 受锁快照:锁内拷 shared_ptr,放锁返回。send_stream 与五口 const 查询
    // 共用,mutable 供 const 侧加锁。
    std::shared_ptr<lubancode::api::Backend> SnapshotInner() const;

    mutable std::mutex mutex_;
    std::shared_ptr<lubancode::api::Backend> inner_;
};

}  // namespace lubancode::app
