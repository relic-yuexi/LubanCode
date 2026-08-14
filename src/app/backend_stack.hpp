// 会话后端栈:按 wire 造真实 client(BuildBackend),再按固定次序包若干层
// 请求改写器——切 provider 不换引用(RebuildableBackend)、/model 与 /think
// 即时生效(Model/ThinkOverrideBackend)、模型目录专属指令与"魂"追加进
// 系统提示(压轴次序见各类注释)、延迟工具索引段(tool_search)、最外层
// 起停"思考中"转轮(SpinnerBackend)。
//
// 这一层只认 api/config/agent/cli 的既有抽象,不 include 交互会话的东西;
// SpinnerBackend 依赖真终端的 cli::Spinner(实现编在可执行文件一侧),
// 链接 lubancode_core 的单测不要构造它,其余各层可直测。

#pragma once

#include <atomic>
#include <expected>
#include <functional>
#include <memory>
#include <string>

#include "agent/prompts.hpp"
#include "api/anthropic/client.hpp"
#include "api/backend.hpp"
#include "api/chat/client.hpp"
#include "api/responses/client.hpp"
#include "cli/spinner.hpp"
#include "cli/theme.hpp"
#include "config/config.hpp"
#include "config/model_catalog.hpp"
#include "config/provider_catalog.hpp"

namespace lubancode::app {

// 按 wire 造对应的后端实现。agent 层只认 Backend 这个抽象接口,不关心
// 背后具体是哪个协议在干活。
inline std::unique_ptr<lubancode::api::Backend> BuildBackend(const lubancode::config::Config& config) {
    // M11:连接超时 / 流式空闲读超时用 Config 里实际生效的值(四级合并结果,
    // 没配就是内置默认值),不是每次都硬编码默认值。
    const auto headers = lubancode::config::ResolveProviderHeaderTemplates(config.extra_headers,
                                                                            config.auth_token);
    if (config.wire == lubancode::config::Wire::Responses) {
        return std::make_unique<lubancode::api::responses::ResponsesBackend>(
            config.base_url, config.auth_token, config.connect_timeout_ms, config.stream_idle_timeout_secs,
            config.native_web_search, config.extra_body, headers);
    }
    if (config.wire == lubancode::config::Wire::ChatCompletions) {
        return std::make_unique<lubancode::api::chat::ChatCompletionsBackend>(
            config.base_url, config.auth_token, config.connect_timeout_ms, config.stream_idle_timeout_secs,
            config.extra_body, headers);
    }
    return std::make_unique<lubancode::api::anthropic::AnthropicBackend>(
        config.base_url, config.auth_token, config.connect_timeout_ms, config.stream_idle_timeout_secs,
        config.native_web_search, config.extra_body, headers);
}

// 会话里切 provider 时，外层 Model/Think/Soul 等包装器、AgentLoop 和工具
// 都还握着 Backend&。这一层把真正的 client 藏起来并按需替换，引用地址
// 不变；下一次请求自然落到新 base_url/wire/key 上。
class RebuildableBackend : public lubancode::api::Backend {
public:
    explicit RebuildableBackend(const lubancode::config::Config& config) { Rebuild(config); }

    void Rebuild(const lubancode::config::Config& config) { inner_ = BuildBackend(config); }

    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request& request,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        return inner_->send_stream(request, on_event, cancel);
    }

private:
    std::unique_ptr<lubancode::api::Backend> inner_;
};

// 包一层 Backend:真正发请求前,把 Request.model 换成"当前会话实际在用的
// model"。AgentLoop 的 model 是构造时定死的私有成员,没有 setter(agent 层
// 现有文件不让动,详见任务规矩),这层包装是唯一能让 /model 切换在下一次
// 请求"真正生效"、又不用碰 agent/loop.hpp/.cpp 的办法。current_model 用
// shared_ptr,是因为 /model 命令改的和这里读的得是同一块内存,AgentLoop
// 只认引用、包装器要跨多轮 Run() 存活。
class ModelOverrideBackend : public lubancode::api::Backend {
public:
    ModelOverrideBackend(lubancode::api::Backend& inner, std::shared_ptr<std::string> current_model)
        : inner_(inner), current_model_(std::move(current_model)) {}

    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request& request,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        lubancode::api::Request patched = request;
        patched.model = *current_model_;
        return inner_.send_stream(patched, on_event, cancel);
    }

private:
    lubancode::api::Backend& inner_;
    std::shared_ptr<std::string> current_model_;
};

// 包一层 Backend:真正发请求前,把 Request.reasoning_effort 换成"当前会话
// 实际在用的推理强度"。跟 ModelOverrideBackend 是同一个套路,同样的理由
// (AgentLoop 没有 setter,agent 层现有文件不让动)——current_think 为空串
// 就是"不发这个参数",维持原有行为不变。/think 命令改的和这里读的是
// 同一块 shared_ptr<string> 内存,单发模式(AskOnce)没有 /think 命令,
// current_think 构造后就不再变,等价于"直接按配置发一次"。
class ThinkOverrideBackend : public lubancode::api::Backend {
public:
    ThinkOverrideBackend(lubancode::api::Backend& inner, std::shared_ptr<std::string> current_think,
                         std::shared_ptr<std::string> current_model,
                         const lubancode::config::ModelCatalog* catalog)
        : inner_(inner),
          current_think_(std::move(current_think)),
          current_model_(std::move(current_model)),
          catalog_(catalog) {}

    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request& request,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        lubancode::api::Request patched = request;
        patched.reasoning_effort = *current_think_;
        if (catalog_ != nullptr) {
            const auto body = lubancode::config::ThinkLevelExtraBody(
                catalog_->FindBySlug(*current_model_), *current_think_);
            for (auto it = body.begin(); it != body.end(); ++it) {
                patched.extra_body[it.key()] = it.value();
            }
        }
        return inner_.send_stream(patched, on_event, cancel);
    }

private:
    lubancode::api::Backend& inner_;
    std::shared_ptr<std::string> current_think_;
    std::shared_ptr<std::string> current_model_;
    const lubancode::config::ModelCatalog* catalog_ = nullptr;
};

// 包一层 Backend:真正发请求前,把模型目录(models.json)里当前模型的
// base_instructions 作为独立段追加到 Request.system 末尾。跟 Model/
// ThinkOverrideBackend 同一个套路、同一个理由(AgentLoop 的系统提示构造时
// 定死,agent 层现有文件不动)——current_instructions 为空串就是"不追加",
// 原样透传,零破坏。/model 切换改的和这里读的是同一块 shared_ptr<string>
// 内存,切到目录外模型时上层把它清空,旧模型的指令自然不再发。
class ModelInstructionsBackend : public lubancode::api::Backend {
public:
    ModelInstructionsBackend(lubancode::api::Backend& inner, std::shared_ptr<std::string> current_instructions)
        : inner_(inner), current_instructions_(std::move(current_instructions)) {}

    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request& request,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        lubancode::api::Request patched = request;
        patched.system = lubancode::agent::WithModelInstructions(request.system, *current_instructions_);
        return inner_.send_stream(patched, on_event, cancel);
    }

private:
    lubancode::api::Backend& inner_;
    std::shared_ptr<std::string> current_instructions_;
};

// 包一层 Backend:真正发请求前,把当前的"魂"(SOUL.md / souls/ 的风格叠加
// 层)追加到 Request.system 最后。跟上面几层同一个套路、同一个理由。魂
// 必须压轴——所以这一层要放在 ModelInstructionsBackend 的更内侧(请求先
// 经过 instructions 层追加模型专属段,再到这层追加魂),字符串里魂自然
// 排最后。current_soul 存的是魂文件的原始内容(注释由 agent::WithSoul 在
// 注入时剥),/soul 切换改的和这里读的是同一块 shared_ptr<string> 内存,
// 下一轮请求即时生效;空串(SOUL.md 默认、或 /soul off)= 不追加,零破坏。
class SoulOverlayBackend : public lubancode::api::Backend {
public:
    SoulOverlayBackend(lubancode::api::Backend& inner, std::shared_ptr<std::string> current_soul)
        : inner_(inner), current_soul_(std::move(current_soul)) {}

    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request& request,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        lubancode::api::Request patched = request;
        patched.system = lubancode::agent::WithSoul(request.system, *current_soul_);
        return inner_.send_stream(patched, on_event, cancel);
    }

private:
    lubancode::api::Backend& inner_;
    std::shared_ptr<std::string> current_soul_;
};

// tool_search(延迟挂载):包一层 Backend,真正发请求前把"延迟未加载"工具
// 的紧凑索引段追加到 Request.system 末尾。跟 ModelInstructionsBackend 同一个
// 套路、同一个理由(AgentLoop 的系统提示构造后改不了,agent 层现有构造不
// 破)——index_provider 每次 send_stream 现算,tool_search 命中后的下一次
// 请求,新挂载的工具自然从索引段里消失;provider 给空串就原样透传,零破坏。
// 这层只包给主 AgentLoop 用,不进 AgentTool 拿的那条链——子代理的索引段
// 按它自己的注册表算,由 AgentTool::SetDeferredIndexProvider 单独注入,
// 两边各管各的,不会重复追加。
class DeferredIndexBackend : public lubancode::api::Backend {
public:
    DeferredIndexBackend(lubancode::api::Backend& inner, std::function<std::string()> index_provider)
        : inner_(inner), index_provider_(std::move(index_provider)) {}

    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request& request,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        lubancode::api::Request patched = request;
        patched.system = lubancode::agent::WithDeferredToolsIndex(
            request.system, index_provider_ ? index_provider_() : std::string());
        return inner_.send_stream(patched, on_event, cancel);
    }

private:
    lubancode::api::Backend& inner_;
    std::function<std::string()> index_provider_;
};

// 包一层 Backend:发起真正的网络请求前起一个"思考中"转轮(cli::Spinner),
// 收到第一个流事件就停。转轮跟着 send_stream 这一次调用走——AgentLoop 一次
// Run() 里可能因为工具调用来回好几趟,每趟各自单独调一次 send_stream,
// 工具执行发生在两次 send_stream 之间(loop.cpp 里,不在这层包装范围内),
// 天然满足"工具执行期间不转,发下一轮请求再转"这条要求,不用改
// agent/loop.cpp 一个字。spinner_enabled 由调用方按"stdout 是不是真控制台"
// 算好传进来——管道模式下这层直接透传,不起线程、不输出任何转轮字符。
class SpinnerBackend : public lubancode::api::Backend {
public:
    SpinnerBackend(lubancode::api::Backend& inner, const lubancode::cli::Theme& theme, bool spinner_enabled)
        : inner_(inner), theme_(theme), spinner_enabled_(spinner_enabled) {}

    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request& request,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        lubancode::cli::Spinner spinner(theme_, spinner_enabled_);
        bool stopped = false;
        const auto wrapped = [&](const lubancode::api::StreamEvent& event) {
            if (!stopped) {
                spinner.Stop();
                stopped = true;
            }
            on_event(event);
        };
        return inner_.send_stream(request, wrapped, cancel);
        // spinner 在这里析构,Stop() 兜底再调一次也是安全的(空操作)——
        // 万一 send_stream 直接失败、一个事件都没吐(比如连都没连上),
        // 转轮不会一直转着。
    }

private:
    lubancode::api::Backend& inner_;
    const lubancode::cli::Theme& theme_;
    bool spinner_enabled_;
};

}  // namespace lubancode::app

