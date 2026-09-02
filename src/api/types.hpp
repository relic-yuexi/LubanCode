// api 层的中立类型:不带任何厂商字眼(不提 Anthropic、不提 MiniMax)。
// anthropic/、responses/ 两个后端各自把厂商私有的 JSON 结构翻译成这里的类型,
// 翻译完的东西对 agent 层长得一模一样。

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/reasoning.hpp"

#include "platform/text_encoding.hpp"  // SanitizeExternalText:消息内容上 wire 前的编码关口
#include "tools/tool_content.hpp"      // ToolContentBlock:工具结果的富内容块(MCP 富结果单)

namespace lubancode::api {

// anthropic wire 必填 max_tokens 时的公开兜底:模型/provider/配置三级都
// 没声明输出上限才用它(见 Request::max_tokens 注释)。与
// config::kDefaultRequiredMaxOutputTokens、agent::kUnsetOutputReserveEstimateTokens
// 同值同注释规矩——改一处须三处一起改,且这不是"把 4096 换一枚更大的
// 魔数":它是兜底声明,配置/目录声明永远压过它。
inline constexpr int kRequiredMaxOutputTokensFallback = 8192;

// ---------------------------------------------------------------------------
// 内容块
// ---------------------------------------------------------------------------

// 一段纯文本。
struct TextBlock {
    std::string text;
};

// 用户附上的图片。data 存不带 data URL 前缀的 base64，方便两套 wire 各按
// 自家的格式包一层；filename/宽高只给本地界面、会话存档和导出展示用。
struct ImageBlock {
    std::string media_type;
    std::string data;
    std::string filename;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// 模型输出的图片(Responses 的 image_generation_call 一类),解码校验后
// 落进会话 artifact 目录的引用。与上面那只用户输入的 ImageBlock 分家:
// 这里只有引用,没有 base64——session/export/resume 只存这份引用,续聊
// 重放时翻成 ModelImageReplayText 的短文本标记,绝不把图片正文塞回请求。
// filename/path 由本地按内容寻址起名,不信模型正文一个字。
struct ModelImageBlock {
    std::string id;         // wire 侧 item id(image_generation_call 的 id),重复终帧的去重键
    std::string filename;   // "<sha256>.png":落盘文件名(内容寻址,本地起的)
    std::string path;       // 相对会话目录的落盘路径("artifacts/sha256/<sha256>.png")
    std::string mime_type;  // 解码后按魔数判定的 MIME("image/png" 等)
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t bytes = 0;  // 解码后字节数
    std::string sha256;     // 解码后正文的 sha256
};

// 模型发起的一次工具调用请求。
struct ToolUseBlock {
    std::string id;
    std::string name;
    nlohmann::json input;  // 工具入参,一个 JSON 对象
    // 调用方标识(动态工具 P3·§7.2):Anthropic 的 programmatic tool calling
    // 会在 tool_use 块上带 caller(如 "code_execution_20260120"),标记这枚
    // 调用发自服务端沙箱而非模型直呼。中立层只保存不解释;空 = wire 没给
    //(绝大多数请求都没有),四家 wire 里只有 anthropic 会填。
    std::string caller;
};

// 工具执行完,把结果回传给模型。MCP 富结果单起 blocks/structured_content
// 与 payload 同构:blocks 为空 = 旧文本路(一切行为与从前一致);非空 =
// 富结果,content 是它的 TextProjection(四家 wire 的文本降级、终端显示、
// token 估算都吃这份投影)。base64 不进这层——图片/音频/资源字节先落
// 会话 artifact,块里只有 ArtifactRef。
struct ToolResultBlock {
    std::string tool_use_id;
    std::string content;  // 富结果的文本投影;文本结果就是原文
    bool is_error = false;
    std::vector<tools::ToolContentBlock> blocks;              // 空 = 纯文本结果
    std::optional<nlohmann::json> structured_content;  // MCP structuredContent;nullopt = 没给
};

// 模型的思考过程(extended thinking / reasoning)。text 是思考正文,
// signature 是 Anthropic extended thinking 的签名——续会话重放历史时
// thinking 块必须带 signature,否则第二轮会被服务端以 400 拒掉。
struct ThinkingBlock {
    std::string text;
    std::string signature;
};

// 服务端执行的工具调用(动态工具 P3·Claude NativeReference)。Anthropic 的
// server tool(工具搜索等)由 provider 自己执行,响应里给的是 server_tool_use
// 块 + 对应的 ServerToolResultBlock;宿主只保存事实、原样回传,绝不在本地
// 再执行一遍(与 BuiltinToolStart 同一条纪律)。web_search 一类 server tool
// 的块不在此列——只有本单声明的工具搜索链路解析进这只块,其余照旧忽略。
struct ServerToolUseBlock {
    std::string id;    // "srvtoolu_..."(与 result 的 tool_use_id 配对)
    std::string name;  // "tool_search_tool_regex" / "tool_search_tool_bm25"
    nlohmann::json input = nlohmann::json::object();  // 搜索入参(pattern/query/limit)
};

// 服务端工具搜索的搜索结果块(wire 名 tool_search_tool_result)。content 存
// 原始嵌套 JSON——tool_search_tool_search_result.tool_references[](内含
// tool_reference)或 tool_search_tool_result_error(error_code/error_message),
// 无损保存、无损回传(单子 §7.2:不压成普通文本再让下一轮猜回来)。
struct ServerToolResultBlock {
    std::string tool_use_id;  // 对应 ServerToolUseBlock.id
    nlohmann::json content = nlohmann::json::object();
};

using ContentBlock =
    std::variant<TextBlock, ImageBlock, ToolUseBlock, ToolResultBlock, ThinkingBlock, ModelImageBlock,
                 ServerToolUseBlock, ServerToolResultBlock>;

// 历史重放时模型输出图片的替身文案(四家 wire 共用):让模型记得自己
// 出过一张图,但不把 base64 塞回请求。
inline std::string ModelImageReplayText(const ModelImageBlock& block) {
    std::string size;
    if (block.width > 0 || block.height > 0) {
        size = " (" + std::to_string(block.width) + "x" + std::to_string(block.height) + ")";
    }
    return "[模型已生成图片: " + block.filename + size + "]";
}

// 工具结果图片的明降级附注(工具结果图片回喂单):wire 上带不动图片字节
// 时(chat completions 的 tool 消息、Gemini 3 之前的模型),在投影文本后
// 追加这一行——点名张数与落盘路径,不冒充发过、不静默吞图。没有图片块
// 的结果返回空串,一个字节不多加。
std::string ToolResultImageDegradedNote(const ToolResultBlock& result);

// ---------------------------------------------------------------------------
// 消息
// ---------------------------------------------------------------------------

enum class Role { User, Assistant };

struct Message {
    Role role = Role::User;
    std::vector<ContentBlock> content;
};

// 把一块内容里的文本字段全部过一遍 SanitizeExternalText(合法时零成本
// 原样返回)。wire 序列化前的编码关口:坏串(会话旧档、管道输入、外部
// 文本)到这里就该被洗掉,不许漏到四个后端去触发 type_error.316 兜底。
// 工具入参/结果是 JSON 树,洗它要走 SanitizeMessage 那层,别在这一块
// 里硬解。
void SanitizeContentBlock(ContentBlock& block);

// 把一条消息的所有内容块清洗一遍(含 ToolUseBlock.input / ToolResultBlock
// 等 JSON 树字段的递归清洗)。合法时零成本,不会改动任何内容。
void SanitizeMessage(Message& message);

// 工具定义的暴露方式(动态工具 P3·§7.1):Deferred 表示这枚定义走 provider
// 的按需加载——完整定义仍随每份请求发出(目录/catalog 仍是唯一事实源),
// 但 anthropic wire 会标 defer_loading:true,provider 把它排除在缓存前缀
// 之外,模型须经服务端工具搜索发现后才见全文。中立层不掺厂商字眼;不认这
// 个概念的 wire(chat/responses/gemini)照旧全量发送,不得擅自丢定义。
enum class ToolLoadMode { Eager, Deferred };

// 工具定义,交给模型看的 JSON Schema。M1 阶段 Request::tools 恒为空,
// 这里先把形状定好,留给 M2 用。
struct ToolDefinition {
    std::string name;
    std::string description;
    nlohmann::json input_schema;
    ToolLoadMode load_mode = ToolLoadMode::Eager;
};

// 工具 schema 上 wire 前的归一化。四家 wire(chat/responses/gemini/anthropic)
// 都得过这道手,原样照抄就是把病带出门。
//
// 缘起:不收参数的工具图省事回了个空对象 {},里头既没 type 也没 properties。
// 宽松端收得下,严格端(OpenAI 档、DeepSeek 随之)按 type 取值取了个空,当场
// 回 "schema must be a JSON Schema of 'type: \"object\"', got 'type: null'"。
// 工具表是整份发的,一件不合规整轮请求被拒——模型的面都见不着。
//
// 更要紧的是野 schema 的来路:MCP server、插件 manifest、Lua 脚本自己声明的
// schema 都不受本仓管束(mcp_tool.cpp / plugin_loader.cpp / lua_tool.cpp 三处
// 都是外来直传)。指望每个来路都守规矩不如在出门这道关上统一兑正。
//
// 兑法从宽:认不得的形状(非 object、缺 type、type 不是 "object")一概兑成
// 最小合法壳 {"type":"object","properties":{}};已经合规的原样放行,不动人家
// 的 required/additionalProperties/嵌套声明。properties 那只空壳也得给——有的
// 严格档校验器认了 type 还要认 properties。
inline nlohmann::json ToolSchemaForWire(const nlohmann::json& schema) {
    const bool declared_object = schema.is_object() && schema.contains("type") &&
                                 schema["type"].is_string() && schema["type"].get<std::string>() == "object";
    if (!declared_object) {
        return nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}};
    }
    if (!schema.contains("properties") || !schema["properties"].is_object()) {
        nlohmann::json fixed = schema;
        fixed["properties"] = nlohmann::json::object();
        return fixed;
    }
    return schema;
}

// ---------------------------------------------------------------------------
// 请求
// ---------------------------------------------------------------------------

struct Request {
    std::string model;
    std::string system;
    std::vector<Message> messages;
    // 输出上限。nullopt = unset:协议允许省略的 wire(chat/responses)整个
    // 不带这个字段,交服务端/模型默认(子代理同级单根因一:旧版三处写死
    // 4096,reasoning 模型一思考就撞墙)。anthropic 协议必填,client 落
    // 公开兜底 kRequiredMaxOutputTokensFallback,不藏魔数;想改上限走
    // 配置 agent.max_output_tokens / 模型目录声明,不走改源码。
    std::optional<int> max_tokens;
    std::vector<ToolDefinition> tools;  // M1 先留空位,不填
    // 服务端工具搜索声明(动态工具 P3):非空("regex"/"bm25")= 请求的 tools
    // 里声明一枚由 provider 执行的工具搜索工具,anthropic wire 映成
    // tool_search_tool_regex_20251119 / tool_search_tool_bm25_20251119。
    // 这是模型级能力(目录声明 deferred_tools + deferred_tool_mode=
    // native_reference 才置),跟请求档案走(/model 切档即随换),不进
    // backend 构造参数——同一 provider 的模型清单里支持的与不支持的各有
    // 各的判词。其他 wire 不认这个字段,恒空。
    std::string server_tool_search;
    // M6.6:推理强度,none/low/medium/high,空串 = 不发这个参数(维持原有
    // 行为)。responses wire 翻成 "reasoning":{"effort":...};anthropic wire
    // 翻成 "thinking":{"type":"enabled","budget_tokens":...}(none 翻成
    // "type":"disabled"),映射关系见 anthropic/client.cpp 里的
    // BuildThinkingJson 注释。
    std::string reasoning_effort;
    // 当前模型声明的推理控制能力。空 = 旧式兼容路径；非空时，各 wire
    // 按规范字段翻译，不借 request.extra_body 偷渡档位或开关。
    ReasoningConfig reasoning;
    // 跨 Turn 保留式思考的中立意图(Kimi 保留式思考单 P1):All 时 Chat
    // 家方言声明了 history_control=thinking_keep 的模型落 thinking.keep
    // 并把 replay 升为 Always;方言没声明的模型一概不发这个形状——不把
    // K2.6 的 keep 状态硬带给 K3/K2.5。与 reasoning_effort 是两笔账:
    // 档位管本轮想多深,history 管跨轮保留。
    ReasoningHistoryMode reasoning_history = ReasoningHistoryMode::ProviderDefault;
    // 调用方显式给出的请求级私有参数。推理档位不走这里；provider 级
    // extra_body 先合并，这里后合并，同名顶层键由请求覆盖。
    nlohmann::json extra_body = nlohmann::json::object();
};

// 一次代理运行所用的请求档案。主会话每次发送前现取当前值；后台任务在
// 派出时抄成快照。两条路都用 ApplyRequestProfile 落进 Request，免得各自
// 再写一套 model / effort / reasoning 覆盖规矩。
struct RequestProfile {
    std::string model;
    std::string reasoning_effort;
    ReasoningConfig reasoning;
    // 跨 Turn 保留意图随档案走(P1):/think history 的会话选择经
    // SyncAgentRequestPolicy 落进来,下一份请求即时生效;子代理/后台任务
    // 派出时抄成快照,与 effort 同一套覆盖规矩。
    ReasoningHistoryMode reasoning_history = ReasoningHistoryMode::ProviderDefault;
    // 服务端工具搜索的变体声明(动态工具 P3):随档案走,与 Request 同名
    // 字同一语义——模型级能力,/model 切档时由装配层按目录重算后随
    // SyncAgentRequestPolicy 整份刷新,空 = 本模型不声明。
    std::string server_tool_search;
};

// model 为空时保留 Request 原值（后台测试和少数调用方会沿用 loop 模型）；
// effort 与 reasoning 总按档案覆盖，空值也算明示“不发推理参数”。
void ApplyRequestProfile(Request& request, const RequestProfile& profile);

// 整份请求的 UTF-8 出门关。所有真实 backend 在拼 wire JSON 前都要过
// 一遍：system/messages/tool schema/extra_body 连同各枚标识一并清洗。
// AgentLoop 里的早期清洗仍留着，用来治驻留历史；这里管所有绕过 loop 的
// 后台请求（标题、压缩、记忆抽取等），免得哪条支路漏洗便触发 dump 316。
void SanitizeRequest(Request& request);

// ---------------------------------------------------------------------------
// 流式事件
// ---------------------------------------------------------------------------

// usage 的统一口径(前缀缓存守恒单,2026-08):三家 wire 翻到这层时必须
// 摊成同一副语义,不许同一字段在一家表示"非缓存输入"、在另一家表示
// "输入总数":
//
//   input_tokens          本次未从缓存读取、按普通输入处理的 token
//   cache_read_tokens     从缓存读取的输入 token(读命中)
//   cache_creation_tokens 本次写缓存的输入 token(provider 有此概念才非 0)
//   output_tokens         输出 token
//   output_reasoning_tokens 输出 token 里属于 reasoning 的部分(含在
//               output_tokens 里,不是另加的一笔;服务端拆了账才非 0——
//               chat wire 的 completion_tokens_details.reasoning_tokens、
//               responses wire 的 output_tokens_details.reasoning_tokens;
//               没拆就是 0,不许拿 0 冒充"reasoning 为零")
//
// 各 wire 的映射(细节在各自 events.cpp):
//   anthropic   input_tokens 本来就不含 cache read/creation,原样照抄;
//   chat        DeepSeek 顶层 prompt_cache_hit/miss_tokens:input=miss,
//               cache_read=hit;OpenAI/Qwen 风格 prompt_tokens_details.
//               cached_tokens:cache_read=cached,input=max(prompt-cached,0);
//               都没有:input=prompt_tokens,cache_read=0;
//   responses   cache_read=input_tokens_details.cached_tokens,
//               input=max(input_tokens-cached_tokens,0)。
struct Usage {
    std::int64_t input_tokens = 0;
    std::int64_t output_tokens = 0;
    std::int64_t cache_read_tokens = 0;
    std::int64_t cache_creation_tokens = 0;
    std::int64_t output_reasoning_tokens = 0;
};

// 完整输入(非缓存输入 + 缓存读 + 缓存写)的唯一算法。UI/统计/汇总一律
// 走这只 helper,不许各家各算——DeepSeek 49k hit + 1k miss 就该是 50k,
// 不是 50k+49k,也不是 1k。
inline std::int64_t TotalInputTokens(const Usage& usage) {
    return usage.input_tokens + usage.cache_read_tokens + usage.cache_creation_tokens;
}

// 一次独立模型请求的 usage 报告:on_usage 的入参(前缀缓存守恒单)。
// usage 之外带上这笔账的身份——第几步、哪个请求、什么模型、哪个缓存
// epoch、这一步前缀是不是上一份的原样追加版——逐步流水账(StepUsageRecord)
// 才有键可落,整轮汇总才能按 token 求和而不是拿百分比平均。
// provider_response_id/model 取流里 MessageStart 的值,provider 不给就是空。
// 注意它是 provider 的 response id,只作外部对账;Journal 关联主键用
// AgentLoop 发请求前铸的 local request id(Token 账本单 §6.1.2),二者不许
// 混——local id 由 runtime RequestUsageRecord 包着,不塞进 api 层。
// cache_epoch/epoch_break_reason/prefix_append_only 由 AgentLoop 的前缀
// 记账(agent/prefix.hpp)在发请求前填:epoch 断不是失败,无名无姓地断
// 才是失败。
struct UsageReport {
    Usage usage;
    int step_index = 0;              // Run() 内的步号(0-based,一步一次模型请求)
    std::string provider_response_id;  // 服务端消息 id(MessageStart.id),可空
    std::string model;               // MessageStart.model,可空
    int cache_epoch = 1;             // 请求落在哪个缓存 epoch(1 起)
    std::string epoch_break_reason;  // 本步断了 epoch 时的点名(空 = 没断)
    bool prefix_append_only = true;  // 本步请求是否上一份的原样追加版

    // ---- 每请求缓存诊断账(问题 9):本地前缀视角,不含任何正文 ----
    // 全部由 AgentLoop 的前缀记账(agent/prefix.hpp 的指纹)在发请求前
    // 填好;一路经事件流到 ContextTracker 的逐请求账。只留短 hash、
    // 长度与枚举,不落 prompt 正文、密钥或完整 URL(单子验收明文)。
    bool epoch_first_request = false;  // 本 epoch 首份请求,没有可比的前一份
    std::string system_hash;           // system 指纹(16 hex,显示层截 8)
    std::string tools_hash;            // 工具定义指纹(16 hex,显示层截 8)
    // 稳定消息前缀:与上一份请求逐条相等的那段开头消息。prefix_hash 是
    // 这段前缀的合成指纹(空串 = 没有稳定前缀,首请求/一条都不共享);
    // stable_prefix_messages 是它的条数,total_messages 是本次请求消息
    // 总数(追加律成立时二者之差就是尾部新添的条数)。
    std::string prefix_hash;
    std::size_t stable_prefix_messages = 0;
    std::size_t total_messages = 0;
    // wire 序列化公共前缀字节数(诊断模式 LUBANCODE_DEBUG_PREFIX 才算,
    // 默认关——不做全序列化):-1 = 没开诊断或该 backend 不提供序列化。
    std::int64_t wire_common_prefix_bytes = -1;

    // provider 是否明报了 usage(Token 账本单 A0 的显式位):四家 wire 看到
    // usage 帧才置真,明报全零也是真。耐久账(accounting::UsageSample 与
    // Trajectory v2 model.usage.recorded)只认这一位。
    bool reported_by_provider = false;
    // provider 是否明报缓存读明细。与 usage 明报分开：usage 对象存在但
    // cached_tokens/cache hit 字段缺席时为 false，显示与耐久账据此写“未报缓存”。
    bool cache_reported_by_provider = false;

    // legacy 推断 helper(五项任一非零 = 报过):老 UI 流水(TurnUsageStats/
    // ContextTracker 面板)沿用。provider"明报全零"与"没报"靠它分不开,
    // 新账路不许再走这只。
    bool reported() const {
        return usage.input_tokens > 0 || usage.output_tokens > 0 || usage.cache_read_tokens > 0 ||
               usage.cache_creation_tokens > 0 || usage.output_reasoning_tokens > 0;
    }
};

// 流的第一个事件,标记消息开始。
struct MessageStart {
    std::string id;
    std::string model;
};

// 文本内容的增量片段,一到就能往屏幕上写。
struct TextDelta {
    std::string text;
};

// 思考过程的增量片段。text 是思考正文的一段,signature 是 Anthropic
// extended thinking 签名的一段(signature_delta)。chat/responses wire
// 没有 signature,signature 字段恒空。流式拼 + 复用 ContentBlockDone 收尾,
// 不加 ThinkingStart。
struct ThinkingDelta {
    std::string text;
    std::string signature;
};

// 一次工具调用开始:拿到 id 和工具名,入参还没填。
struct ToolUseStart {
    int index = 0;
    std::string id;
    std::string name;
    std::string caller;  // PTC 的调用方标识(anthropic wire 才有,空 = 没给)
};

// 服务端工具搜索的调用开始(动态工具 P3):provider 执行的搜索,宿主只攒
// 事实(入参走 ToolUseInputDelta 按 index 累积),不在本地执行。收到即知
// 这枚 tool_use 是服务端的,与本地执行的 ToolUseStart 分家。
struct ServerToolUseStart {
    int index = 0;
    std::string id;
    std::string name;
};

// 服务端工具搜索的搜索结果(动态工具 P3):流式下整只块随 content_block_start
// 一次到齐(官方流样例如此),没有增量;content 是嵌套结果的原始 JSON
//(tool_references 或 error),无损入历史。
struct ServerToolResult {
    int index = 0;
    std::string tool_use_id;
    nlohmann::json content = nlohmann::json::object();
};

// 工具调用入参的增量片段(JSON 字符串,要靠调用方自己拼完整再解析)。
struct ToolUseInputDelta {
    int index = 0;
    std::string partial_json;
};

// 某个内容块结束。
struct ContentBlockDone {
    int index = 0;
};

// Responses 等协议的服务端内置工具。它已由模型服务执行，客户端只展示
// 轨迹，绝不能塞进 ToolUseBlock 再本地执行一遍。
struct BuiltinToolStart {
    std::string id;
    std::string name;
    nlohmann::json input = nlohmann::json::object();
};

struct BuiltinToolDone {
    std::string id;
    std::string name;
    nlohmann::json input = nlohmann::json::object();
    std::string summary;
    bool is_error = false;
};

// 流的最后一个语义事件:消息结束,带上停止原因和用量统计。
// usage_reported(Token 账本单 A0):provider 是否真回了一帧 usage。四家 wire
// 只在帧里真的出现 usage 对象时置真——"明报全零"与"压根没报"从这里分家,
// 下游不许再拿"五项全零"猜。老解析路径没置位的,消费端按 legacy 推断。
struct MessageDone {
    std::string stop_reason;
    Usage usage;
    bool usage_reported = false;
    bool cache_reported = false;  // wire 中是否真出现 cache token 明细字段
};

// 模型输出的一张图片(Responses 的 image_generation_call.result)。base64
// 只在这只事件里流转:解码、校验、落 artifact 是宿主(agent 层)的活,做完
// 换成 ModelImageBlock 引用入历史——base64 不进 assembler、不进 session。
// 同一张图可能随 output_item.done 与 response.completed 各到一次(重复
// 终帧),消费端按 id 去重。
struct ImageOutput {
    std::string id;      // image_generation_call 的 item id
    std::string base64;  // 图片正文(base64,不带 data: 前缀)
};

// 流里出现的错误(服务端主动报错,或者本地解析出的问题)。
struct StreamError {
    std::string message;
    std::string code;  // provider 的稳定业务错误码(没有则空)
};

using StreamEvent = std::variant<MessageStart, TextDelta, ThinkingDelta, ToolUseStart, ToolUseInputDelta,
                                 ContentBlockDone, BuiltinToolStart, BuiltinToolDone, MessageDone, ImageOutput,
                                 ServerToolUseStart, ServerToolResult, StreamError>;

// ---------------------------------------------------------------------------
// 错误
// ---------------------------------------------------------------------------

enum class ErrorKind {
    Network,     // 连不上、断线之类
    HttpStatus,  // HTTP 状态码非 2xx
    Parse,       // JSON / SSE 解析不动
    Api,         // 服务端返回的业务错误(error 事件)
    Cancelled,   // 用户按 ESC 主动打断,不是真出错——调用方不该当错误报给用户
};

struct Error {
    ErrorKind kind = ErrorKind::Network;
    std::string message;
    int http_status = 0;  // kind == HttpStatus 时才有意义
    std::string api_code;  // kind == Api 时的 provider 稳定错误码(没有则空)
};

// 给人看的错误体摘要(ccmoon 真机巡检单 P1):HTTP 非 2xx 的原始响应体
// 可能是整段 JSON,直接糊脸既看不清也容易夹密钥。这里抽出 error 里的
// message/type/code(有啥抽啥),拼成一行;疑似密钥(sk-/Bearer 尾巴)
// 打码,超 240 字节截短标省略。不是 JSON 就按原文走同一道打码截短。
std::string SummarizeErrorBodyForUser(const std::string& body);

// 鉴权三态(向导重排单):按 token 组装请求基础头。token 非空给 Content-Type
// + Authorization;token 为空(无鉴权,或 env 缺值)只给 Content-Type,彻底
// 不发 Authorization——绝不发一枚空 Bearer 冒充无鉴权。三套正式 client
// (anthropic/responses/chat)与 ListModels 同吃这一份,header 行为单测钉在这。
inline std::map<std::string, std::string> RequestBaseHeaders(const std::string& auth_token) {
    std::map<std::string, std::string> headers{{"Content-Type", "application/json"}};
    if (!auth_token.empty()) {
        headers["Authorization"] = "Bearer " + auth_token;
    }
    return headers;
}

// extra_headers 覆盖/追加到一份基础 HTTP 头表:值非空 = 覆盖/追加同名头,
// 空值 = 删除该头(key 精确匹配大小写;真正发送时 cpr::Header 自身还会再做
// 一层大小写不敏感的去重,这里只管"配置里写的头名对不对得上基础头哪一条"
// 这一步)。批六归一的共用件:四家 wire 都拿它合头;anthropic/responses 的
// 同名公开函数(单测钉着名字)是这份的薄壳。
inline std::map<std::string, std::string> ApplyExtraHeaders(std::map<std::string, std::string> base,
                                                             const std::map<std::string, std::string>& extra_headers) {
    for (const auto& [name, value] : extra_headers) {
        if (value.empty()) base.erase(name);
        else base[name] = value;
    }
    return base;
}

// Role -> wire 角色名。user 一角四家都叫 "user";另一角各家叫法不同
// (anthropic/responses 叫 assistant,gemini 叫 model),由调用方给。chat
// wire 不走这个(它的 role 集合另有 system/tool,直拼字符串)。
inline std::string RoleToString(Role role, std::string_view other_role) {
    return role == Role::User ? std::string("user") : std::string(other_role);
}

// extra_body 顶层浅合并(wire 请求体拼装的共同收尾):source 是 object 才
// 合并,同名键整个覆盖,不深合并。provider 级与请求级(Request::
// extra_body)各调一次,后者压前者。gemini 的 generationConfig 一键深一层
// 是它自家的特例,不走这个。
inline void MergeExtraBody(nlohmann::json& body, const nlohmann::json& source) {
    if (!source.is_object()) {
        return;
    }
    for (auto it = source.begin(); it != source.end(); ++it) {
        body[it.key()] = it.value();
    }
}

}  // namespace lubancode::api
