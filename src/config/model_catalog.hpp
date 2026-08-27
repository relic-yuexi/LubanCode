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
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/reasoning.hpp"

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
    // 内置条目来自哪家 provider；用户 ~/.lubancode/models.json 留空，
    // 仍可按 slug 全局覆盖。
    std::string provider_id;
    std::string slug;
    std::string display_name;
    std::string description;
    std::string default_think;
    std::vector<ThinkLevel> supported_think_levels;
    lubancode::api::ReasoningConfig reasoning;
    std::string base_instructions;
    std::optional<std::size_t> context_window_tokens;
    // 输出上限声明("8k"/裸数字,解析时换算):三级声明的最底一级
    // (agent::ResolveOutputBudget),规格根因一。nullopt = 未声明。
    std::optional<std::size_t> max_output_tokens;
    std::optional<bool> supports_parallel_tool_calls;  // 暂不启用
    std::vector<std::string> input_modalities;          // 暂不启用
    std::string truncation_policy;                       // 暂不启用
    // 端点能力(ccmoon 巡检单 P1):自 provider catalog 的 capabilities 抄
    // 来,用户 models.json 条目也可自写。认的键见 ClassifyModelEndpoint
    //(realtime/image-generation/reasoning/...),目录没写的键当没声明。
    // 这不是"模型会不会干活"的判词,只给 /model 的端点相性提示用。
    std::map<std::string, bool> capabilities;
};

// 模型与请求端点的相性分类(纯函数):catalog 条目的 capabilities 与模型
// 名合判。Realtime 模型(gpt-*-realtime-* 一类)吃的是 Realtime/WebSocket
// 端点,LubanCode 的四家 wire 没有一家走得通——它们混进 /model 菜单时挂
// 醒目标记、确认前说清"多半不走当前 wire"。判词的边界:这只说"当前
// wire 大概率不通",不判模型死刑,也不判那家中转的 Realtime 路由通不通
//(巡检单:Realtime 报错只证明这家中转的 Responses 路由不通)。
enum class ModelEndpointKind {
    Standard,   // 普通 chat/responses 模型
    Realtime,   // Realtime 端点模型(catalog realtime 能力或名字带 realtime)
    ImageGen,   // 只出图的模型(catalog image-generation 且不吃 reasoning)
};
ModelEndpointKind ClassifyModelEndpoint(const ModelCatalogEntry* entry, const std::string& model_id);

// 思考关闭声明(MiniCPM5 真机巡检单 P1):目录条目 capabilities 里的
// `always_think` 或 `off_unsupported` 任一为真,就声明"这个模型的思考关
// 不掉"。/think none 照旧把关闭请求发出去(不硬塞私有模板参数),但切换
// 前后都要明说"此端点未证实可关",别让状态栏只挂一枚 none 便算数。
// 目录没写这两键 = Unknown,不猜。
enum class ThinkOffDeclaration { Unknown, DeclaredUnsupported };
ThinkOffDeclaration ClassifyThinkOffDeclaration(const ModelCatalogEntry* entry);

// 图片输入能力(MiniCPM5 真机巡检单 P2):input_modalities 与 capabilities
// 合判。声明了吃图(text/image 列表含 image,或 capabilities.image = true)
// = Multimodal;声明了纯文本(input_modalities 只有 text,或
// capabilities.image = false)= TextOnly——/image 与 @路径的附件在发送前
// 拦住,等服务端回 500 就晚了。目录没声明 = Unknown,允许试探(发送后
// 由服务端错误分型兜底)。audio 键同法可查,留给音频路接线时用。
enum class ImageInputSupport { Unknown, TextOnly, Multimodal };
ImageInputSupport ClassifyImageInputSupport(const ModelCatalogEntry* entry);

// 整份目录。source_path 空串 = 磁盘上没有这份文件(空目录,不算错)。
// warnings 是解析时跳过的坏东西,一条一句人话,启动时打给用户看。
struct ModelCatalog {
    std::vector<ModelCatalogEntry> models;
    std::string source_path;
    std::vector<std::string> warnings;

    // 按 slug(API 模型名)精确查条目,没有返回 nullptr。重名取先出现的。
    const ModelCatalogEntry* FindBySlug(const std::string& slug) const;
    const ModelCatalogEntry* FindByProviderAndSlug(const std::string& provider,
                                                   const std::string& slug) const;
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

// 活列表选择落痕(跨家判定第三轮返件):/model 切成的模型在 provider_id
// 家落一条用户条目 {"slug","provider_id","display_name"} 进
// models_json_path(调用方传 <主目录>/.lubancode/models.json)。幂等:同
// slug 且同 provider_id 的条目已在就原样返回,条目上已有字段(effort
// 声明、窗口、指令)一个不许冲;文件不存在从头建,坏 JSON 报错不写,
// 绝不覆盖用户手写的目录。条目字段见 ParseEntry:provider_id 自此可选。
std::expected<void, std::string> RememberModelChoiceInCatalog(const std::string& models_json_path,
                                                              const std::string& provider_id,
                                                              const std::string& slug,
                                                              const std::string& display_name);

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
