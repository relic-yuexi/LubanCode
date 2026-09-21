// 模型协议窄合同(FD-05 模型协议窄合同从配置总头剥离):四家 API client
// (anthropic/responses/chat/gemini 的 client.hpp)与 ListModels(api/
// models.hpp)跟模型对话只认两样东西——Wire 枚举与四枚网络超时默认值。
// 它们原先住在 config/config.hpp,逼得 API 公共头整份加载配置合同
// (Config/FileConfig 连同 hooks/channels/telemetry 段全跟着进来):
// 配置总头一动,模型接口相关编译单元跟着重编,API↔config 也结成目录层
// 反向边。搬到本头之后,API 层只引这枚零依赖的小头;config.hpp include
// 它,单一真源不另抄常量。解析函数(ParseProviderWire/ProviderWireName)
// 是配置字符串层的事,仍住总头(config.hpp/config.cpp),不随枚举搬——
// API 层只认枚举,不解析配置串。

#pragma once

namespace lubancode::config {

// 说哪种"方言"跟模型对话:Anthropic 的 Messages API、OpenAI 的 Responses
// API、兼容面最广的 OpenAI Chat Completions,还是 Google Gemini 的
// Generate Content API。默认 anthropic。
//
// 命名规范(wire 更名单,2026-08):配置里的规范名跟业内叫法
// 对齐——anthropic-messages / openai-responses / openai-chat-completions /
// google-generate-content,ProviderWireName 一律吐这四个;旧名(anthropic /
// responses / chat_completions / chat)在 ParseProviderWire 里永久当别名
// 认,老配置文件不许崩。枚举成员名保持 Anthropic/Responses/ChatCompletions
// 原样不动——配置字符串层换规范名是一回事,源码里几十处 Wire:: 引用(含
// provider_catalog 等旁人正在改的文件)没必要陪着翻一遍。
enum class Wire { Anthropic, Responses, ChatCompletions, GoogleGenerateContent };

// M11(网络超时):两个 API 客户端(anthropic/responses)以及 ListModels 共用
// 的超时默认值,毫秒/秒两种单位混用是因为对应的 cpr 选项本身单位不同
// (ConnectTimeout 认毫秒,LowSpeed::time 和 cpr::Timeout 的语义按秒/毫秒
// 分别处理更直观,这里 idle/request 两个字段存"秒",内部换算成毫秒喂给 cpr)。
//
// - kDefaultConnectTimeoutMs:TCP+TLS 握手阶段的上限。连不上时(DNS 解析
//   不动、服务器不回包)靠这个及时报错,不干等。15 秒是"网络稍差也能连上,
//   真连不上也不用等太久"的折中。
// - kDefaultStreamIdleTimeoutSecs:流式(SSE)读空闲超时——不是总时长上限
//   (流式回答本可以很长),是"连续这么多秒一个字节都没收到"就判定连接
//   假死。60 秒给足模型长时间思考/工具调用间隙的余量,又不至于真断线时
//   干等太久。
// - kDefaultRequestTimeoutSecs:非流式请求(目前只有 ListModels 拉模型
//   列表)的整体超时——这类请求响应体小,没有"回复很长"的顾虑,直接给
//   总时长上限。30 秒跟 tools/web_search.cpp、tools/web_fetch.cpp 里已有的
//   cpr::Timeout{30000} 保持一致的量级。
// - kDefaultRequestHardTimeoutSecs(cpr 并发挂死单):每枚**流式**请求的
//   硬墙钟——与上面三个是两码事。connect/idle 两道闸只管"连接阶段"与
//   "连续无字节";真机现场(本机代理/TUN 截胡 127.0.0.1 回环)出现过
//   请求进了 cpr::Post 再不返、两道闸都不触发的挂死,唯一兜底是给整枚
//   请求一面不可穿透的墙:ProgressCallback 里对 steady_clock 比期限,
//   超期返回 false 掐流(libcurl 周期性调它,连接死寂也醒)。不能用
//   cpr::Timeout 实现——那是 CURLOPT_TIMEOUT,会把正常的长流拦腰砍断。
//   默认 300s 给足长思考/长回复的余量;0 = 不设这道墙(回到旧行为)。
constexpr int kDefaultConnectTimeoutMs = 15000;
constexpr int kDefaultStreamIdleTimeoutSecs = 60;
constexpr int kDefaultRequestTimeoutSecs = 30;
constexpr int kDefaultRequestHardTimeoutSecs = 300;

}  // namespace lubancode::config
