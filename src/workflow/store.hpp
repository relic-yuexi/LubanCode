// Workflow Store 与 ${...} resolver(自然语言编排单第 2 批)。
//
// Store 是有名分区(单子"图的核心模型"Store 一节):
//   inputs   本次运行输入,只读
//   vars     显式变量(首版无 assign 节点,预留)
//   nodes.<id>.output   节点终态输出,只写一次(CommitOutput 原子落账)
//   nodes.<id>.meta     attempt、耗时、错误、缓存命中等
//   artifacts.<id>      大文件引用(id/mime/size/hash),正文走 ArtifactStore
//   run     workflow/run/version/cwd 等只读元数据
//
// 并行分支各写自己名下;CommitOutput 对同一节点只许一次,第二次报错——
// 这是"进程崩在 Execute 中途,恢复后不会看见半份 output"的账面基础。
//
// Resolver:${inputs.topic}、${nodes.arxiv.output.items} 只从已完成节点
// 读值;解析失败指到节点、字段与期望(错误带点路径)。

#pragma once

#include <expected>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "workflow/definition.hpp"

namespace lubancode::workflow {

// ${...} 解析错误。
struct ResolveError {
    std::string path;     // 出错的引用原文("${nodes.x.output.a}" 或字段路径)
    std::string message;  // 指到节点、字段与期望
};

// 一枚值:JSON 或字面量文本混排的结果。ResolveTemplate 处理"一段文本里
// 埋多个 ${...}"的形态;整值引用(值就是引用本身)保持 JSON 类型不落
// 字符串——"${nodes.x.output}" 指到数组时下游拿到的是数组,不是串。
struct ResolvedValue {
    nlohmann::json value;
    bool from_single_ref = false;  // 整值就是单个引用(保持类型)
};

class Store {
public:
    Store() = default;

    // 开场:填 inputs 与 run 元数据。只许调一次。
    void Initialize(const nlohmann::json& inputs, const nlohmann::json& run_meta);

    // 节点输出落账(原子,只写一次)。同 id 二次提交返回 false——恢复
    // 路径的账面护栏。
    bool CommitOutput(const std::string& node_id, const nlohmann::json& output);
    // 循环迭代里的重提交(第 N 轮覆写第 N-1 轮):正常执行路径用,
    // meta.iteration 由 runtime 记。返回 true=首写,false=覆写。
    bool CommitOutputOverwrite(const std::string& node_id, const nlohmann::json& output);
    // 节点 meta 合并(可多次;attempt/duration/error 一类)。
    void UpdateMeta(const std::string& node_id, const nlohmann::json& meta);
    // 恢复路径:journal 重放时已有 output 的节点直接落账(等价 CommitOutput)。
    bool RestoreOutput(const std::string& node_id, const nlohmann::json& output) {
        return CommitOutput(node_id, output);
    }

    bool HasOutput(const std::string& node_id) const;
    std::optional<nlohmann::json> GetOutput(const std::string& node_id) const;
    std::optional<nlohmann::json> GetMeta(const std::string& node_id) const;

    const nlohmann::json& inputs() const { return inputs_; }
    const nlohmann::json& run_meta() const { return run_meta_; }

    // 序列化(checkpoint 用):整本账的稳定形状。
    nlohmann::json ToJson() const;
    // 恢复:字段逐搬(mutex 不可动,移动构造手写)。
    static Store FromJson(const nlohmann::json& j);

    Store(Store&& other) noexcept { TakeFrom(std::move(other)); }
    Store& operator=(Store&& other) noexcept {
        if (this != &other) TakeFrom(std::move(other));
        return *this;
    }
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

private:
    void TakeFrom(Store&& other);

    mutable std::mutex mutex_;
    bool initialized_ = false;
    nlohmann::json inputs_ = nlohmann::json::object();
    nlohmann::json run_meta_ = nlohmann::json::object();
    std::map<std::string, nlohmann::json> outputs_;
    std::map<std::string, nlohmann::json> metas_;
};

// ${...} 引用取值(单引用,无混排)。认 inputs./vars./nodes.<id>.output|
// meta./run./artifacts. 前缀;nodes.<id> 后面必须还有 .output/.meta 段。
// 路径段逐级下钻,数组按下标。失败给 ResolveError。
std::expected<nlohmann::json, ResolveError> ResolveRef(const Store& store, const std::string& ref);

// 模板/对象解析:字符串里混 ${...} 的,替换成字符串形式;整值单引用的,
// 保持 JSON 类型;对象/数组递归。节点 input 的 ResolveInputs 就走它。
std::expected<ResolvedValue, ResolveError> ResolveTemplate(const Store& store, const nlohmann::json& input);

}  // namespace lubancode::workflow
