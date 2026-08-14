#include "tools/agent_tool.hpp"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <memory>
#include <sstream>
#include <utility>
#include <variant>

#include "agent/loop.hpp"
#include "agent/prompts.hpp"
#include "tools/path_utils.hpp"

namespace lubancode::tools {

namespace {

std::string SubAgentPersona() {
    return "你是 general-purpose 子代理,能搜索、分析并完成多步任务。专注给定任务,完成后直接给出结论,不要寒暄。";
}

std::string ExplorePersona() {
    return "你是 Explore 子代理,专门快速搜索、阅读并分析代码库。只读,不得改文件、启动会改动环境的命令或做别的写操作。"
           "完成后给出简明结论和具体文件位置,不要寒暄。";
}

std::string ExtractLastText(const agent::AgentLoop& loop) {
    const auto& history = loop.History();
    if (history.empty()) {
        return std::string();
    }
    std::string text;
    for (const auto& block : history.back().content) {
        if (std::holds_alternative<api::TextBlock>(block)) {
            text += std::get<api::TextBlock>(block).text;
        }
    }
    return text;
}

class DetachedRequestBackend : public api::Backend {
public:
    explicit DetachedRequestBackend(DetachedAgentBackend& detached) : detached_(detached) {}

    std::expected<void, api::Error> send_stream(
        const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        api::Request patched = request;
        if (!detached_.model.empty()) {
            patched.model = detached_.model;
        }
        patched.reasoning_effort = detached_.reasoning_effort;
        for (auto it = detached_.request_extra_body.begin(); it != detached_.request_extra_body.end(); ++it) {
            patched.extra_body[it.key()] = it.value();
        }
        return detached_.backend->send_stream(patched, on_event, cancel);
    }

private:
    DetachedAgentBackend& detached_;
};

bool ExploreAllows(const Tool& tool) {
    const std::string name = tool.name();
    return name == "read_file" || name == "search" || name == "web_fetch" || name == "web_search" ||
           name == "lsp";
}

// ---------------------------------------------------------------------------
// isolation=worktree 的 base_dir 包装层(0.27.x)
//
// 子代理是进程内线程,共享进程 cwd——绝不能 chdir(会把主会话的读写全带
// 进沟里,多个隔离子代理并行时更互相踩脚)。做法是不动各工具内部,在建
// 子代理工具表时套一层装饰:路径入参按房解析成绝对路径,run_command 注入
// 房作为工作目录;三道闸(文件/cwd/git 改道)由工具自身按线程本地的隔离
// 范围栈(IsolationGuard,AgentLoop 跑动前压入)执行。
// ---------------------------------------------------------------------------

class BaseDirTool : public Tool {
public:
    BaseDirTool(Tool& inner, IsolationScope scope) : inner_(inner), scope_(std::move(scope)) {}

    std::string name() const override { return inner_.name(); }
    std::string description() const override { return inner_.description(); }
    nlohmann::json input_schema() const override { return inner_.input_schema(); }
    bool needs_confirm() const override { return inner_.needs_confirm(); }
    bool deferred() const override { return inner_.deferred(); }

    Result execute(const nlohmann::json& input) override {
        nlohmann::json patched = input;
        const std::string inner_name = inner_.name();
        if (inner_name == "read_file" || inner_name == "write_file" || inner_name == "edit_file" ||
            inner_name == "search") {
            const auto it = patched.find("path");
            if (it != patched.end() && it->is_string()) {
                const std::string path = it->get<std::string>();
                if (!path.empty() && !Utf8ToPath(path).is_absolute()) {
                    patched["path"] = scope_.base_dir + "/" + path;
                }
            }
        } else if (inner_name == "run_command") {
            if (patched.find("cwd") == patched.end()) {
                patched["cwd"] = scope_.base_dir;
            }
        }
        return inner_.execute(patched);
    }

private:
    Tool& inner_;
    IsolationScope scope_;
};

// 把一张工具表整体包成"落在房里"的表。包装件按引用持内层工具,源表必须
// 活得比返回的表久(前台是会话级子表;后台是线程 lambda 里的局部序)。
std::unique_ptr<ToolRegistry> BuildIsolatedRegistry(ToolRegistry& source, const IsolationScope& scope) {
    auto out = std::make_unique<ToolRegistry>();
    for (const auto& tool : source.All()) {
        out->Register(std::make_unique<BaseDirTool>(*tool, scope));
    }
    return out;
}

}  // namespace

AgentTool::AgentTool(api::Backend& backend, ToolRegistry& sub_registry, std::string cwd, std::string model,
                      int default_max_turns, std::string skills_segment)
    : backend_(backend),
      sub_registry_(sub_registry),
      cwd_(std::move(cwd)),
      model_(std::move(model)),
      default_max_turns_(default_max_turns),
      skills_segment_(std::move(skills_segment)) {}

AgentTool::~AgentTool() {
    {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        for (const auto& task : tasks_) {
            task->cancel.store(true, std::memory_order_release);
        }
    }
    for (auto& thread : task_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

std::string AgentTool::name() const {
    return "agent";
}

std::string AgentTool::description() const {
    return "把独立任务委托给子代理。agent_type=Explore 是只读代码搜索代理;general-purpose 能研究、执行多步任务和改代码。"
           "子代理有独立上下文,只把结论交回主对话。run_in_background=true 时任务在会话后台运行,本次调用立刻返回任务编号,"
           "适合与主线无依赖的摸排;主线马上需要结论时传 false。后台任务不能弹权限确认,未预先放行的操作会被拒绝。"
           "子代理看不见当前对话历史,prompt 必须自包含。";
}

nlohmann::json AgentTool::input_schema() const {
    nlohmann::json schema = nlohmann::json::object();
    schema["type"] = "object";

    nlohmann::json properties = nlohmann::json::object();

    nlohmann::json prompt_prop = nlohmann::json::object();
    prompt_prop["type"] = "string";
    prompt_prop["description"] =
        "交给子代理的任务描述,必须自包含——子代理看不见主对话历史,任务目标、范围、期望的输出形式都要"
        "写清楚。";
    properties["prompt"] = prompt_prop;

    nlohmann::json max_turns_prop = nlohmann::json::object();
    max_turns_prop["type"] = "integer";
    max_turns_prop["description"] =
        "子代理最多跑几轮(每轮一次工具调用来回算一轮),不填默认 40;传 0 = 不设上限。";
    properties["max_turns"] = max_turns_prop;

    nlohmann::json type_prop = nlohmann::json::object();
    type_prop["type"] = "string";
    type_prop["enum"] = nlohmann::json::array({"general-purpose", "Explore"});
    type_prop["description"] = "子代理类型:Explore 只读搜索分析;general-purpose 可做多步操作。默认 general-purpose。";
    properties["agent_type"] = type_prop;

    nlohmann::json background_prop = nlohmann::json::object();
    background_prop["type"] = "boolean";
    background_prop["description"] = "是否放到会话后台运行。独立摸排用 true;当前回答离不开结果时用 false。";
    properties["run_in_background"] = background_prop;

    nlohmann::json isolation_prop = nlohmann::json::object();
    isolation_prop["type"] = "string";
    isolation_prop["enum"] = nlohmann::json::array({"none", "worktree"});
    isolation_prop["description"] =
        "worktree = 给子代理单独开一间 git worktree 隔离房干活:写不碰主 checkout(文件/命令/git 三道闸拦),"
        "干完没改动房自动删,有改动则保留并在结果里附房路径与分支,由主代理或用户收尾。"
        "改代码的多步任务建议带上;只读摸排不必。缺省 none。";
    properties["isolation"] = isolation_prop;

    schema["properties"] = properties;
    schema["required"] = nlohmann::json::array({"prompt"});

    return schema;
}

Tool::Result AgentTool::execute(const nlohmann::json& input) {
    if (!input.contains("prompt") || !input.at("prompt").is_string()) {
        return {"缺少必填参数 prompt(字符串)", true};
    }
    const std::string prompt = input.at("prompt").get<std::string>();
    if (prompt.empty()) {
        return {"prompt 不能是空字符串", true};
    }

    if (const auto it = input.find("agent_type"); it != input.end() && !it->is_string()) {
        return {"agent_type 得是字符串", true};
    }
    if (const auto it = input.find("run_in_background"); it != input.end() && !it->is_boolean()) {
        return {"run_in_background 得是布尔值", true};
    }
    std::string agent_type = input.value("agent_type", std::string("general-purpose"));
    if (agent_type == "explore") {
        agent_type = "Explore";
    }
    if (agent_type != "general-purpose" && agent_type != "Explore") {
        return {"agent_type 只认 general-purpose 或 Explore", true};
    }

    std::string isolation = input.value("isolation", std::string("none"));
    if (isolation != "none" && isolation != "worktree") {
        return {"isolation 只认 none 或 worktree", true};
    }
    if (isolation == "worktree" && agent_type == "Explore") {
        return {"Explore 是只读代理,用不上 worktree 隔离(isolation 去掉或换 general-purpose)", true};
    }
    const bool isolate = isolation == "worktree";

    int max_turns = default_max_turns_;
    if (const auto it = input.find("max_turns"); it != input.end() && !it->is_null()) {
        if (!it->is_number_integer()) {
            return {"max_turns 得是整数", true};
        }
        max_turns = it->get<int>();
        if (max_turns < 0) {
            return {"max_turns 不能是负数(0 = 不设上限)", true};
        }
    }

    ToolRegistry& task_registry =
        agent_type == "Explore" && explore_registry_ != nullptr ? *explore_registry_ : sub_registry_;
    const bool background = input.value("run_in_background", background_by_default_);
    if (background) {
        return LaunchBackground(input, agent_type, task_registry, max_turns, isolate);
    }
    return ExecuteForeground(input, agent_type, task_registry, max_turns, isolate);
}

std::optional<cli::AgentWorktree> AgentTool::SetupIsolationRoom(Result& error_out) {
    const std::filesystem::path cwd = Utf8ToPath(cwd_);
    const auto repo_root = cli::FindRepositoryRoot(cwd, git_runner_);
    if (!repo_root.has_value()) {
        error_out = {"isolation=worktree 需要在 git 仓库里给子代理建房,当前目录不是仓库: " + cwd_, true};
        return std::nullopt;
    }
    cli::AgentWorktree room = cli::CreateAgentWorktree(*repo_root, git_runner_);
    if (!room.ok) {
        error_out = {"给隔离子代理建 worktree 失败: " + room.error, true};
        // 半拉子房收拾掉,不留垃圾。
        if (!room.room_path.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(room.room_path, ec);
        }
        return std::nullopt;
    }
    return room;
}

std::string AgentTool::FinishIsolationRoom(const cli::AgentWorktree& room, const cli::GitRunner& runner) {
    return cli::FinishAgentWorktree(room.repo_root, room.room_path, room.branch, runner).note;
}

Tool::Result AgentTool::ExecuteForeground(const nlohmann::json& input, const std::string& agent_type,
                                          ToolRegistry& task_registry, int max_turns, bool isolate) {
    // isolation=worktree:建房、锁房、工具表套 base_dir 包装、隔离范围压栈,
    // 跑完收工(干净删房,有活留房附路径)。cwd 一根指头都不动。
    std::optional<cli::AgentWorktree> room;
    std::unique_ptr<ToolRegistry> isolated_registry;
    std::optional<ScopedIsolation> scope_guard;
    std::optional<IsolationScope> scope_storage;
    if (isolate) {
        Result setup_error;
        room = SetupIsolationRoom(setup_error);
        if (!room.has_value()) {
            return setup_error;
        }
        scope_storage = IsolationScope{room->name, PathToUtf8(room->room_path), PathToUtf8(room->repo_root)};
        isolated_registry = BuildIsolatedRegistry(task_registry, *scope_storage);
        scope_guard.emplace(*scope_storage);
    }
    ToolRegistry& effective_registry = isolated_registry != nullptr ? *isolated_registry : task_registry;

    const Hooks hooks = hooks_;
    Result result = RunTask(backend_, effective_registry, input.at("prompt").get<std::string>(), agent_type,
                            max_turns, &hooks, nullptr, /*detached=*/nullptr,
                            /*prepared_system_prompt=*/nullptr,
                            scope_storage.has_value() ? &*scope_storage : nullptr);
    if (room.has_value()) {
        result.content += FinishIsolationRoom(*room, git_runner_);
    }
    return result;
}

Tool::Result AgentTool::LaunchBackground(const nlohmann::json& input, const std::string& agent_type,
                                         ToolRegistry& task_registry, int max_turns, bool isolate) {
    if (!detached_backend_factory_) {
        return {"当前入口没有配置后台子代理后端,请把 run_in_background 设为 false", true};
    }
    // isolation=worktree:主线程里把房建好、锁上,建不成同步报错——后台
    // 任务没人可问,失败要立刻回给模型。房信息带进线程,收工清理。
    std::optional<cli::AgentWorktree> room;
    if (isolate) {
        Result setup_error;
        room = SetupIsolationRoom(setup_error);
        if (!room.has_value()) {
            return setup_error;
        }
    }

    // 已收尾的 std::thread 若一直不 join，系统线程句柄会跟着会话一路攒。
    // 状态变成终态后，线程只剩 TouchTasks() 一步，挨个收柄不会拖住界面。
    for (std::size_t i = 0; i < task_threads_.size(); ++i) {
        bool finished = false;
        {
            std::lock_guard<std::mutex> lock(tasks_mutex_);
            finished = i < tasks_.size() && tasks_[i]->snapshot.state != AgentTaskState::Running;
        }
        if (finished && task_threads_[i].joinable()) {
            task_threads_[i].join();
        }
    }
    {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        constexpr std::size_t kMaxRunningTasks = 8;
        const std::size_t running = static_cast<std::size_t>(
            std::count_if(tasks_.begin(), tasks_.end(), [](const auto& task) {
                return task->snapshot.state == AgentTaskState::Running;
            }));
        if (running >= kMaxRunningTasks) {
            return {"后台子代理已跑满 8 路，请等一项收尾后再开", true};
        }
    }

    DetachedAgentBackend detached;
    std::unique_ptr<ToolRegistry> detached_registry;
    try {
        detached = detached_backend_factory_();
        detached_registry = detached_registry_factory_ ? detached_registry_factory_() : nullptr;
    } catch (const std::exception& error) {
        return {"后台子代理初始化失败: " + std::string(error.what()), true};
    } catch (...) {
        return {"后台子代理初始化失败: 未知错误", true};
    }
    if (!detached.backend) {
        return {"后台子代理后端创建失败", true};
    }

    auto task = std::make_shared<TaskRecord>();
    {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        task->snapshot.id = next_task_id_++;
        task->snapshot.agent_type = agent_type;
        task->snapshot.prompt = input.at("prompt").get<std::string>();
        task->snapshot.state = AgentTaskState::Running;
        task->snapshot.start_time = std::chrono::steady_clock::now();
        tasks_.push_back(task);
    }
    TouchTasks();
    const int id = task->snapshot.id;
    const std::string prompt = task->snapshot.prompt;
    std::string system_prompt = agent::BuildSystemPrompt(
        cwd_, agent_type == "Explore" ? ExplorePersona() : SubAgentPersona(),
        agent_type == "Explore" ? std::string() : skills_segment_, prompts_dir_, project_instructions_);
    system_prompt += "\n\n这是后台任务。启动目录是 " + cwd_ +
                     "。调用文件与搜索工具时一律传绝对路径；不要依赖进程当前目录，它可能随主会话切换。";
    system_prompt = agent::WithModelInstructions(system_prompt, detached.model_instructions);
    system_prompt = agent::WithSoul(system_prompt, detached.soul);
    ToolRegistry* registry = detached_registry != nullptr ? detached_registry.get() : &task_registry;
    task_threads_.emplace_back([this, task, registry, prompt, agent_type, max_turns,
                                detached = std::move(detached),
                                system_prompt = std::move(system_prompt),
                                detached_registry = std::move(detached_registry),
                                room = std::move(room)]() mutable {
        (void)detached_registry;  // 让独立工具表活到线程收尾
        // isolation=worktree:线程里包表、压隔离范围,收工清理。包装表按
        // 引用持源表工具,声明在源表之后,析构反序先亡,引用不悬垂。
        std::unique_ptr<ToolRegistry> isolated_registry;
        std::optional<ScopedIsolation> scope_guard;
        std::optional<IsolationScope> scope_storage;
        if (room.has_value()) {
            scope_storage = IsolationScope{room->name, PathToUtf8(room->room_path), PathToUtf8(room->repo_root)};
            isolated_registry = BuildIsolatedRegistry(*registry, *scope_storage);
            scope_guard.emplace(*scope_storage);
        }
        ToolRegistry& effective_registry =
            isolated_registry != nullptr ? *isolated_registry : *registry;
        DetachedRequestBackend backend(detached);
        Result result;
        try {
            result = RunTask(backend, effective_registry, prompt, agent_type, max_turns, nullptr, task, &detached,
                             &system_prompt, scope_storage.has_value() ? &*scope_storage : nullptr);
        } catch (const std::exception& error) {
            result = {"子代理执行失败: " + std::string(error.what()), true};
        } catch (...) {
            result = {"子代理执行失败: 未知错误", true};
        }
        if (room.has_value()) {
            result.content += FinishIsolationRoom(*room, git_runner_);
        }
        {
            std::lock_guard<std::mutex> lock(tasks_mutex_);
            task->snapshot.result = result.content;
            task->snapshot.end_time = std::chrono::steady_clock::now();
            if (task->cancel.load(std::memory_order_acquire)) {
                task->snapshot.state = AgentTaskState::Cancelled;
            } else {
                task->snapshot.state = result.is_error ? AgentTaskState::Failed : AgentTaskState::Done;
            }
        }
        TouchTasks();
    });

    return {"后台子代理 #" + std::to_string(id) + " (" + agent_type + ") 已启动。主会话可以继续；完成结果会在后续回合送达。",
            false};
}

Tool::Result AgentTool::RunTask(api::Backend& backend, ToolRegistry& task_registry, const std::string& prompt,
                                const std::string& agent_type, int max_turns, const Hooks* foreground_hooks,
                                const std::shared_ptr<TaskRecord>& background_task,
                                const DetachedAgentBackend* detached,
                                const std::string* prepared_system_prompt,
                                const IsolationScope* isolation_scope) {
    // tool_search:延迟工具索引段按"此刻的 loaded 集合"现算(provider 里
    // 闭包着 main.cpp 那份 shared_ptr),拼在子代理系统提示末尾。子代理
    // 运行中途自己 tool_search 挂载了新工具,这段索引不会跟着刷新(系统
    // 提示构造后定死)——但 tools 数组每轮现拼(见 AgentLoop 注释),挂载
    // 照样生效,索引段只是稍显陈旧,无害。
    std::string system_prompt;
    if (prepared_system_prompt != nullptr) {
        system_prompt = *prepared_system_prompt;
    } else {
        system_prompt = agent::WithDeferredToolsIndex(
            agent::BuildSystemPrompt(cwd_, agent_type == "Explore" ? ExplorePersona() : SubAgentPersona(),
                                     agent_type == "Explore" ? std::string() : skills_segment_, prompts_dir_,
                                     project_instructions_),
            agent_type == "Explore" ? std::string()
                                      : (deferred_index_provider_ ? deferred_index_provider_() : std::string()));
    }
    if (detached != nullptr && prepared_system_prompt == nullptr) {
        system_prompt = agent::WithModelInstructions(system_prompt, detached->model_instructions);
        system_prompt = agent::WithSoul(system_prompt, detached->soul);
    }
    if (isolation_scope != nullptr) {
        system_prompt += "\n\n本次任务运行在隔离的 git worktree 里: " + isolation_scope->base_dir +
                         "。相对路径一律以这间房为基准(包装层会自动解析);主 checkout 只读——写入、命令"
                         "工作目录、git 改道指回主树的操作都会被拦。改动留在房内,收工自会处置。";
    }
    // 每次 execute() 都是全新的、空历史的子代理——没有跨调用的状态,
    // 子代理内部也不做自动 compact(短命任务用不上),AgentLoop 自带的
    // 字符数硬安全网(TrimHistory)照样生效,不用额外处理。
    const std::string task_model = detached != nullptr && !detached->model.empty() ? detached->model : model_;
    agent::AgentLoop sub_loop(backend, task_registry, task_model, system_prompt, /*max_tokens=*/4096, max_turns);
    if (agent_type == "Explore") {
        sub_loop.SetToolFilter([](const Tool& tool) { return ExploreAllows(tool); });
    } else if (detached == nullptr && tool_filter_) {
        sub_loop.SetToolFilter(tool_filter_);
    }

    agent::Callbacks sub_callbacks;
    if (background_task != nullptr) {
        sub_callbacks.on_text_delta = [this, background_task](const std::string& text) {
            std::lock_guard<std::mutex> lock(tasks_mutex_);
            background_task->snapshot.live_output += text;
            constexpr std::size_t kLiveOutputCap = 64 * 1024;
            if (background_task->snapshot.live_output.size() > kLiveOutputCap) {
                background_task->snapshot.live_output.erase(
                    0, background_task->snapshot.live_output.size() - kLiveOutputCap);
            }
            TouchTasks();
        };
        sub_callbacks.on_tool_start = [this, background_task](const std::string& tool_name,
                                                               const nlohmann::json& tool_input) {
            std::lock_guard<std::mutex> lock(tasks_mutex_);
            background_task->snapshot.tool_calls.push_back(
                AgentTaskToolCall{tool_name, tool_input.dump(), std::string(), false, false});
            TouchTasks();
        };
        sub_callbacks.on_tool_done = [this, background_task](const std::string& tool_name, const Result& result) {
            std::lock_guard<std::mutex> lock(tasks_mutex_);
            for (auto it = background_task->snapshot.tool_calls.rbegin();
                 it != background_task->snapshot.tool_calls.rend(); ++it) {
                if (!it->done && it->name == tool_name) {
                    it->done = true;
                    it->is_error = result.is_error;
                    it->result = result.content;
                    break;
                }
            }
            TouchTasks();
        };
        // 后台没有可停下来问话的终端。需确认的操作一律拒绝，跟 Claude
        // Code 后台 subagent 的权限边界一致。
        sub_callbacks.on_tool_confirm = [](const std::string&, const nlohmann::json&) { return false; };
        sub_callbacks.on_usage = [this, background_task](const api::Usage& usage) {
            std::lock_guard<std::mutex> lock(tasks_mutex_);
            background_task->snapshot.input_tokens += usage.input_tokens;
            background_task->snapshot.output_tokens += usage.output_tokens;
            TouchTasks();
        };
    } else if (foreground_hooks != nullptr) {
        // 前台路径沿用旧回调，既有确认、转录和 usage 记账一个不丢。
        sub_callbacks.on_tool_start = [foreground_hooks](const std::string& tool_name,
                                                          const nlohmann::json& tool_input) {
            if (foreground_hooks->on_sub_tool_start) {
                foreground_hooks->on_sub_tool_start(tool_name, tool_input);
            }
        };
        sub_callbacks.on_tool_confirm = foreground_hooks->on_tool_confirm;
        sub_callbacks.on_usage = foreground_hooks->on_usage;
        sub_callbacks.on_pre_tool_hook = foreground_hooks->on_pre_tool_hook;
        sub_callbacks.on_post_tool_hook = foreground_hooks->on_post_tool_hook;
    }

    // 打断信号透传:hooks_.cancel 是 main.cpp 那份 cancel_flag 的地址,
    // 没设(nullptr)时 AgentLoop::Run 内部判断照旧短路成"没被打断",
    // 行为跟原来一样;设了之后子代理的工具循环才会真的看见 ESC/Ctrl+C。
    const std::atomic<bool>* cancel = background_task != nullptr
                                          ? &background_task->cancel
                                          : (foreground_hooks != nullptr ? foreground_hooks->cancel : nullptr);
    const auto result = sub_loop.Run(prompt, sub_callbacks, cancel);
    if (!result.has_value()) {
        return {"子代理执行失败: " + result.error(), true};
    }

    const std::string text = ExtractLastText(sub_loop);
    if (sub_loop.History().empty()) {
        return {"子代理没有给出任何结论", true};
    }
    if (text.empty()) {
        return {"子代理没有给出文本结论", true};
    }
    return {text, false};
}

std::vector<AgentTaskSnapshot> AgentTool::TaskSnapshots(std::size_t max_entries) const {
    std::lock_guard<std::mutex> lock(tasks_mutex_);
    if (max_entries == 0 || tasks_.size() <= max_entries) {
        std::vector<AgentTaskSnapshot> out;
        out.reserve(tasks_.size());
        for (const auto& task : tasks_) {
            out.push_back(task->snapshot);
        }
        return out;
    }

    std::vector<bool> selected(tasks_.size(), false);
    std::size_t selected_count = 0;
    for (std::size_t i = 0; i < tasks_.size(); ++i) {
        if (tasks_[i]->snapshot.state == AgentTaskState::Running) {
            selected[i] = true;
            ++selected_count;
        }
    }
    for (std::size_t i = tasks_.size(); i > 0 && selected_count < max_entries; --i) {
        if (!selected[i - 1]) {
            selected[i - 1] = true;
            ++selected_count;
        }
    }

    std::vector<AgentTaskSnapshot> out;
    out.reserve(selected_count);
    for (std::size_t i = 0; i < tasks_.size(); ++i) {
        if (selected[i]) {
            out.push_back(tasks_[i]->snapshot);
        }
    }
    return out;
}

bool AgentTool::HasRunningTasks() const {
    std::lock_guard<std::mutex> lock(tasks_mutex_);
    return std::any_of(tasks_.begin(), tasks_.end(), [](const auto& task) {
        return task->snapshot.state == AgentTaskState::Running;
    });
}

bool AgentTool::HasUndeliveredCompletions() const {
    std::lock_guard<std::mutex> lock(tasks_mutex_);
    return std::any_of(tasks_.begin(), tasks_.end(), [](const auto& task) {
        return task->snapshot.state != AgentTaskState::Running && !task->snapshot.delivered;
    });
}

std::string AgentTool::DrainCompletionNotices() {
    std::lock_guard<std::mutex> lock(tasks_mutex_);
    std::ostringstream out;
    for (const auto& task : tasks_) {
        auto& snapshot = task->snapshot;
        if (snapshot.state == AgentTaskState::Running || snapshot.delivered) {
            continue;
        }
        snapshot.delivered = true;
        out << "[后台子代理结果 #" << snapshot.id << " (" << snapshot.agent_type << ", ";
        switch (snapshot.state) {
            case AgentTaskState::Done:
                out << "完成";
                break;
            case AgentTaskState::Failed:
                out << "失败";
                break;
            case AgentTaskState::Cancelled:
                out << "已取消";
                break;
            case AgentTaskState::Running:
                break;
        }
        out << ")]\n" << snapshot.result << "\n";
    }
    return out.str();
}

}  // namespace lubancode::tools
