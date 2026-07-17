// 模型目录(model catalog,借鉴 Codex 的 model-catalog):每个模型一条
// 详细配置,住在 <主目录>/.lubancode/models.json,格式:
//   {"models":[{"slug":"MiniMax-M3", "display_name":..., ...}, ...]}
// 条目字段(除 slug 全部可选):
//   slug                        必填,就是发请求用的 API 模型名,按它查条目
//   display_name                展示名,/model 列表用
//   description                 一句话说明
//   default_think               切到该模型时自动应用的推理强度档位
//   supported_think_levels      [{"effort":"high","description":"..."}, ...],
//                               /think 裸敲时列出来;设了表外档位只提示不拦
//   base_instructions           注入系统提示的模型专属指令(独立段,不碰人格段)
//   context_window              "1m"/"512k"/裸数字,切到该模型时更新会话窗口
//   supports_parallel_tool_calls / input_modalities / truncation_policy
//                               先解析存储、暂不启用(留给后续棒次接线)
//
// 目录是锦上添花,不是硬依赖:文件缺失 = 空目录,一切回退现状;坏 JSON、
// 坏条目只记一条警告跳过,绝不报错拦人——所以解析函数不返回 expected,
// 永远给出一份(可能是空的)目录,警告攒在 warnings 里由启动时打印。
// 解析是纯函数(不碰 IO),LoadModelCatalog 才真读盘,跟 config.hpp 的
// MergeConfig/LoadFromEnv 一个分工规矩。

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace lubancode::config {

// 一个推理强度档位的声明:effort 是发出去的档位串,description 给人看。
struct ThinkLevel {
    std::string effort;
    std::string description;
};

// 目录里的一个模型条目。context_window 在解析时就换算成 token 数
// (ParseContextWindowTokens,"1m"/"512k"/裸数字三种写法都认),换算不动
// 算坏条目。最后三个字段只解析存储,本棒不启用。
struct ModelCatalogEntry {
    std::string slug;
    std::string display_name;
    std::string description;
    std::string default_think;
    std::vector<ThinkLevel> supported_think_levels;
    std::string base_instructions;
    std::optional<std::size_t> context_window_tokens;
    std::optional<bool> supports_parallel_tool_calls;  // 暂不启用
    std::vector<std::string> input_modalities;          // 暂不启用
    std::string truncation_policy;                       // 暂不启用
};

// 整份目录。source_path 空串 = 磁盘上没有这份文件(空目录,不算错)。
// warnings 是解析时跳过的坏东西,一条一句人话,启动时打给用户看。
struct ModelCatalog {
    std::vector<ModelCatalogEntry> models;
    std::string source_path;
    std::vector<std::string> warnings;

    // 按 slug(API 模型名)精确查条目,没有返回 nullptr。重名取先出现的。
    const ModelCatalogEntry* FindBySlug(const std::string& slug) const;
};

// <主目录>/.lubancode/models.json 的路径;找不到主目录返回 std::nullopt。
std::optional<std::string> ModelCatalogPath();

// 纯函数,不碰 IO:解析 models.json 全文。整体坏了(不是合法 JSON、顶层
// 不是 object、models 不是数组)→ 空目录 + 一条警告;单个条目坏了(缺
// slug、字段类型不对、context_window 换算不动……)→ 跳过该条目 + 一条
// 警告,好条目照收。永不失败。file_path_for_error 只拼进警告文案。
ModelCatalog ParseModelCatalogJson(const std::string& json_text, const std::string& file_path_for_error);

// 真读盘:文件不存在 = 空目录(source_path 留空,不算错);存在但打不开
// 或解析出坏东西,警告都在返回值的 warnings 里。
ModelCatalog LoadModelCatalog();

// /think(/effort)裸敲时列的档位行,一档一行("  - 档位  说明")。
// entry 为空指针或没声明 supported_think_levels 时返回空 vector,调用方
// 按现状提示("支持哪些档位以服务商为准")。
std::vector<std::string> ThinkLevelHintLines(const ModelCatalogEntry* entry);

// 某档位是不是在条目声明的档位表里。ASCII 大小写不敏感——跟 anthropic
// 那张 think 映射表一个待遇,免得 High/high 被当成两个档。
bool ThinkLevelDeclared(const ModelCatalogEntry& entry, const std::string& level);

// 启动 / /model 切换时,按目录条目算出"要应用什么"。纯函数,好单测:
//   think / context_window_tokens 有值才应用(条目声明了、且用户没显式
//   配过——启动时 explicit 按 Source 判断,/model 主动切换两个都传 false,
//   目录声明了就应用);
//   base_instructions 永远给出"该生效的那份":模型不在目录、或条目没写,
//   就是空串——调用方拿它整个覆盖会话里的当前值,切到目录外模型时旧
//   模型的指令自然被清掉,回退现状。
struct CatalogApplication {
    bool in_catalog = false;
    std::optional<std::string> think;
    std::optional<std::size_t> context_window_tokens;
    std::string base_instructions;
};

CatalogApplication ComputeCatalogApplication(const ModelCatalog& catalog, const std::string& slug,
                                              bool think_explicitly_configured,
                                              bool window_explicitly_configured);

}  // namespace lubancode::config
