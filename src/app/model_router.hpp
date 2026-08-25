// ModelRouterService(模型分工第一期,app 层):把 config 的三角色配置与
// 会话现场(当前模型/活跃 provider)折成 agent::ModelRouteTable,并按
// TaskKind 交出"路由 + backend"。调用方只报 TaskKind,不自行拼 model
// 字符串(规格"调用点收拢")。
//
// 跨 provider 路由须重建对应 backend(规格"不能只在当前 backend 上换一串
// model 名"):provider 名空或等于活跃端时复用会话的裸 backend(压缩、抽取
// 本来就走它,不经过 Model/Think/Soul 包装链);别的 provider 按其条目现
// 建一只裸 client,按 provider 名缓存,会话生命周期内复用。找不到 provider
// 条目时交出空 backend 指针,调用方按"该任务暂不可发"处理并记账,不许
// 静默换名。
//
// usage 分角色记账也住这:每个调用点收到 usage 后 ledger().Record() 一笔;
// /context 与 /model roles 翻的就是这本账。
//
// 骨架拆解批一·病四:Route 之外加 Sample 一站——"路由 + 采样 + 记账"的
// 小模型活一扇门。简单后台活(记忆抽取这类"路由即发、发完记账"的形状)
// 直接走它;路由策略特殊的调用方(compact 的动态角色回退、microcompact
// 的独占 backend、evaluator 的 goal 侧账)仍用 Route/RouteDetached 拿路由,
// 采样一律走 agent::SampleModel 原语。
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "agent/model_router.hpp"
#include "agent/sample_model.hpp"
#include "api/backend.hpp"
#include "config/config.hpp"

namespace lubancode::app {

class ModelRouterService {
public:
    // config_result:完整配置与来源(/model roles 的"来源"列用)。main_
    // backend:会话的裸 backend(RebuildableBackend),同 provider 路由复用
    // 它的引用;调用方保证存活期覆盖本服务。current_model:/model 切换改的
    // 就是这块内存(shared_ptr),normal 角色跟着会话模型走。
    ModelRouterService(const lubancode::config::ConfigResult& config_result,
                       lubancode::api::Backend& main_backend, std::shared_ptr<std::string> current_model,
                       const std::string& active_provider);

    // 一次路由的结果。backend 为空 = 跨 provider 名字找不到条目(或会话
    // 模型为空),调用方应跳过该后台任务并 RecordFallback/告警,不许拿
    // 当前模型顶包。
    struct Routed {
        lubancode::agent::ModelRoute route;
        lubancode::api::Backend* backend = nullptr;
    };

    // 独占请求专用:按同一路由另造一只裸 backend,不与主会话或别的调用
    // 共用 client。可移进 worker,也可供工具里嵌套发一枚同步请求。
    struct DetachedRouted {
        lubancode::agent::ModelRoute route;
        std::unique_ptr<lubancode::api::Backend> backend;
    };

    // 每次 Route() 现折路由表:配置与会话模型都可能中途变(三级来源、
    // /model 切换),现折最便宜也最诚实。kind 决定角色与 compact 兼容别名。
    Routed Route(lubancode::agent::TaskKind kind) const;

    // Route() 交借用指针,只准主线程回合边界同步用;这一路交独占 backend。
    // provider 找不到时 backend 为空,不偷偷换回当前端。
    DetachedRouted RouteDetached(lubancode::agent::TaskKind kind) const;

    // 同上,但只问"该用谁",不要 backend(诊断、/model roles、预算计算用)。
    lubancode::agent::ModelRoute RouteInfo(lubancode::agent::TaskKind kind) const;

    // Sample 一站(批一·病四):按 TaskKind 路由 → agent::SampleModel 采样
    // → ledger 记账(角色按 DefaultRoleForTask)。model/effort 从路由来,
    // 调用方不再自己拼。backend 落空(跨 provider 名字找不到条目)时不发
    // 不记,调用方按"该任务暂不可发"自行兜底并按旧口径补零账(如有)。
    struct SampleCall {
        std::string system;                    // 调用方拼好的指令
        std::vector<lubancode::api::Message> messages;  // 一般就一条 user
        std::optional<int> max_tokens;         // 空 = 不带上限字段
    };
    struct SampleOutcome {
        lubancode::agent::ModelRoute route;    // 实际用的路由(来源/回退标记齐)
        lubancode::api::Backend* backend = nullptr;  // 空 = 路由落空,没采
        lubancode::agent::SampleResult result; // 采样产物(backend 非空时有效)
        bool recorded = false;                 // ledger 是否已记这笔账
    };
    SampleOutcome Sample(lubancode::agent::TaskKind kind, const SampleCall& call,
                         const lubancode::agent::SampleOptions& options = {}) const;

    // 完整路由表(/model roles 的短表从这里画)。
    lubancode::agent::ModelRouteTable Table() const;

    lubancode::agent::ModelUsageLedger& ledger() const { return ledger_; }

private:
    // 把目标 provider 展开成可直接交 BuildBackend 的运行配置。同步缓存
    // 与独占请求共用这一口,鉴权/header/reasoning 字段不走岔。
    std::optional<lubancode::config::Config> ConfigForProvider(const std::string& provider) const;
    // 跨 provider 的裸 client 缓存。mutable:Route 逻辑上只读,缓存是实现
    // 细节;同步小活仍只在主线程用。会并行或嵌套发出的任务须走
    // RouteDetached,不许借这份缓存并发或重入发请求。
    lubancode::api::Backend* BackendForProvider(const std::string& provider) const;

    const lubancode::config::ConfigResult& config_result_;
    lubancode::api::Backend& main_backend_;
    std::shared_ptr<std::string> current_model_;
    // 引用会话的活跃端名:/provider switch 改的就是那块内存,路由表下一
    // 次 Route() 现读现折,不会拿旧端名建 backend。
    const std::string& active_provider_;
    mutable std::map<std::string, std::unique_ptr<lubancode::api::Backend>> provider_backends_;
    mutable lubancode::agent::ModelUsageLedger ledger_;
};

// 把 ConfigResult 的三角色配置折成三份 ModelRoleSpec(顺序 normal/cheap/
// lao,ResolveModelRoutes 直接收)。高级段 model_roles 优先于 shorthand,
// 来源句写明哪一级;两级都没配的留空 spec(走回退链)。纯函数,单测钉
// 优先级与来源。
std::vector<lubancode::agent::ModelRoleSpec> BuildRoleSpecs(const lubancode::config::ConfigResult& config_result);

// /model roles 的短表(纯函数,单测钉格式):一行一角色,列 = 角色/
// provider/model/effort/来源。回落的角色来源列写"回落到 normal(...)",
// 不把 normal 的名字重印一遍让用户猜(规格"界面"节);model 为空的行
// (会话模型都还没有)来源列明说"未定"。列宽按内容自适应(至少与表头
// 同宽),空格对齐,返回的行不带换行符。
std::vector<std::string> FormatModelRolesTable(const lubancode::agent::ModelRouteTable& table);

}  // namespace lubancode::app
