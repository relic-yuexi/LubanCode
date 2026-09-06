#include "agent/context.hpp"

#include <string>
#include <type_traits>
#include <variant>

#include "agent/runtime_profile.hpp"   // kFallbackContextWindowTokens:窗口未知时的兜底
#include "agent/tool_result_images.hpp"  // EstimateImageTokensForPreflight:图片 token 的像素口径公共尺
#include "platform/text_encoding.hpp"    // Utf8PrefixBoundary:截短不劈半个字

namespace lubancode::agent {

namespace {

std::size_t BlockChars(const api::ContentBlock& block) {
    return std::visit(
        [](const auto& b) -> std::size_t {
            using T = std::decay_t<decltype(b)>;
            if constexpr (std::is_same_v<T, api::TextBlock>) {
                return b.text.size();
            } else if constexpr (std::is_same_v<T, api::ImageBlock>) {
                return b.media_type.size() + b.data.size() + b.filename.size();
            } else if constexpr (std::is_same_v<T, api::ToolUseBlock>) {
                return b.name.size() + b.id.size() + b.input.dump().size();
            } else if constexpr (std::is_same_v<T, api::ToolResultBlock>) {
                return b.tool_use_id.size() + b.content.size();
            } else if constexpr (std::is_same_v<T, api::ThinkingBlock>) {
                return b.text.size() + b.signature.size();
            } else {
                return 0;
            }
        },
        block);
}

std::size_t MessageChars(const api::Message& message) {
    std::size_t total = 0;
    for (const auto& block : message.content) {
        total += BlockChars(block);
    }
    return total;
}

// ---------------------------------------------------------------------------
// 保命索的私有账(公开口径见 context.hpp 的 ShrinkOversizedToolResults)
// ---------------------------------------------------------------------------

// 截断下限(字节,同旧字符轴时代的 kMinKeepChars 值):每条结果至少留这
// 么多原文,免得截成空壳,模型连是什么工具的结果都看不出。
constexpr std::size_t kMinKeepBytes = 1024;
constexpr const char kResultTruncateMark[] = "\n[内容过长已截断]";

// 一条 ToolResultBlock 估多少 token:与 EstimateMessageTokens 的
// ToolResultBlock 分支同一把尺(tool_use_id + content 投影 + 富块图片按
// 像素折)——判线、显示与预检不各拿各的账。
std::size_t ToolResultBlockTokens(const api::ToolResultBlock& block, double calibration) {
    std::size_t image_tokens = 0;
    for (const auto& rich : block.blocks) {
        if (const auto* image = std::get_if<tools::ImageContent>(&rich); image != nullptr) {
            image_tokens += EstimateImageTokensForPreflight(image->width, image->height, image->wire_base64);
        }
    }
    return ApplyTokenCalibration(
        EstimateUtf8Tokens(block.tool_use_id) + EstimateUtf8Tokens(block.content) + image_tokens, calibration);
}

// token 预算内最长前缀的字节长度(码点边界对齐):逐码点扫,ASCII/非 ASCII
// 分开计数,按统一口径折 token,再进一个码点就要超预算即停。估算随前缀
// 单调不减,扫描即准;续字节不单独折算,天然不劈多字节字符。seed_ascii/
// seed_non_ascii 是"正文之外还要拼进同一份估算的固定字符"(尾部标注),
// 预置进账里再扫——截完的整串(正文+标注)才精确落回预算内,不会因整串
// 除法桶合并差出一两个 token。
std::size_t Utf8PrefixBytesWithinTokens(const std::string& text, std::size_t token_budget, double calibration,
                                        std::size_t seed_ascii = 0, std::size_t seed_non_ascii = 0) {
    std::size_t ascii = seed_ascii;
    std::size_t non_ascii = seed_non_ascii;
    std::size_t bytes = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto byte = static_cast<unsigned char>(text[i]);
        if ((byte & 0xC0) == 0x80) {
            ++bytes;  // UTF-8 续字节:码点账在其首字节处已记
            continue;
        }
        const std::size_t ascii_if_taken = ascii + (byte < 0x80 ? 1 : 0);
        const std::size_t non_ascii_if_taken = non_ascii + (byte < 0x80 ? 0 : 1);
        const std::size_t tokens_if_taken =
            ApplyTokenCalibration(ascii_if_taken / 4 + non_ascii_if_taken * 3 / 2, calibration);
        if (tokens_if_taken > token_budget) {
            break;  // 这个码点进不来:前缀到此为止
        }
        ascii = ascii_if_taken;
        non_ascii = non_ascii_if_taken;
        ++bytes;
    }
    return bytes;
}

// 把一段文本截进 token 预算:尾部打标注,至少留 kMinKeepBytes。本来就
// 线内或已是最小保留量(截不动)返回 false。标注的字符账预置进扫描器,
// 截完(id + 正文 + 标注)精确落回线内,不靠"差几个 token 也算过"。刀口
// 先对齐码点边界再砍——裸按字节 resize,砍进三字节汉字的腰上,末尾悬半个
// 字,合法 UTF-8 也会被截成非法,请求体 dump 当场 type_error.316,整场会
// 话每回合必挂(真机上掐死过)。
bool TruncateTextToTokenBudget(std::string& text, std::size_t token_budget, double calibration) {
    const std::size_t mark_size = sizeof(kResultTruncateMark) - 1;
    if (text.size() <= kMinKeepBytes + mark_size) {
        return false;  // 小于最少保留量:没得裁
    }
    // 标注自身的字符账(ASCII/非 ASCII 分开),预置进扫描。
    std::size_t mark_ascii = 0;
    std::size_t mark_non_ascii = 0;
    for (std::size_t i = 0; i < mark_size; ++i) {
        const auto byte = static_cast<unsigned char>(kResultTruncateMark[i]);
        if ((byte & 0xC0) == 0x80) {
            continue;  // 续字节:码点账在首字节处记
        }
        if (byte < 0x80) {
            ++mark_ascii;
        } else {
            ++mark_non_ascii;
        }
    }
    std::size_t keep = Utf8PrefixBytesWithinTokens(text, token_budget, calibration, mark_ascii, mark_non_ascii);
    if (keep < kMinKeepBytes) {
        keep = kMinKeepBytes;  // 最少保留量托底
    }
    keep = platform::Utf8PrefixBoundary(text, keep);
    if (keep + mark_size >= text.size()) {
        return false;  // 预算装得下全文:不动
    }
    text.resize(keep);
    text += kResultTruncateMark;
    return true;
}

}  // namespace

std::size_t EstimateHistoryBytes(const std::vector<api::Message>& history) {
    std::size_t total = 0;
    for (const auto& message : history) {
        total += MessageChars(message);
    }
    return total;
}

std::vector<api::Message> ShrinkOversizedToolResults(std::vector<api::Message> messages,
                                                     std::size_t window_tokens, double calibration,
                                                     TrimReport* report) {
    if (window_tokens == 0) {
        // 窗口未知不裸奔:token 轴有兜底窗口(依据见 runtime_profile.hpp)。
        window_tokens = kFallbackContextWindowTokens;
    }
    const std::size_t per_result_budget =
        window_tokens * static_cast<std::size_t>(kOversizedToolResultWindowPercent) / 100;
    for (auto& message : messages) {
        for (auto& block : message.content) {
            if (!std::holds_alternative<api::ToolResultBlock>(block)) {
                continue;
            }
            auto& tool_result = std::get<api::ToolResultBlock>(block);
            // while 而非 if:富块结果一次截一块(从最后一块文本起倒着),
            // 截完按真账重估,不估"截多少正好"的一次到位账——图片等不可裁
            // 的分量混在里头,精确账算不出,重估最诚实。
            while (ToolResultBlockTokens(tool_result, calibration) > per_result_budget) {
                bool reduced = false;
                if (!tool_result.blocks.empty()) {
                    // MCP 富结果(P0.3 规矩):只裁 TextContent 的 text,
                    // 图片/音频/资源引用与 structuredContent 一概不动;裁完
                    // content 按真账重算投影,缓存与权威不失步。
                    for (auto it = tool_result.blocks.rbegin(); it != tool_result.blocks.rend(); ++it) {
                        auto* text_block = std::get_if<tools::TextContent>(&*it);
                        if (text_block == nullptr) {
                            continue;
                        }
                        // 这块得缩到多少,整条结果才落回线内:线内预算减去
                        // "本条结果里这块以外的一切"(id、别的文本块、图片)。
                        const std::size_t total_tokens = ToolResultBlockTokens(tool_result, calibration);
                        const std::size_t block_tokens = EstimateUtf8Tokens(text_block->text, calibration);
                        const std::size_t others = total_tokens > block_tokens ? total_tokens - block_tokens : 0;
                        const std::size_t target = per_result_budget > others ? per_result_budget - others : 0;
                        if (!TruncateTextToTokenBudget(text_block->text, target, calibration)) {
                            continue;  // 这块已线内/已到下限:换前一块
                        }
                        {
                            tools::ToolResultPayload payload;
                            payload.content = tool_result.blocks;
                            payload.structured_content = tool_result.structured_content;
                            tool_result.content = tools::TextProjection(payload);
                        }
                        reduced = true;
                        break;  // 重估整条结果
                    }
                } else {
                    const std::size_t id_tokens = EstimateUtf8Tokens(tool_result.tool_use_id, calibration);
                    const std::size_t target = per_result_budget > id_tokens ? per_result_budget - id_tokens : 0;
                    reduced = TruncateTextToTokenBudget(tool_result.content, target, calibration);
                }
                if (!reduced) {
                    break;  // 没有可裁的文本(纯图片/全到下限):放行
                }
                if (report != nullptr) {
                    report->truncated_results = true;
                }
            }
        }
    }
    return messages;
}

std::size_t ApplyTokenCalibration(std::size_t tokens, double calibration) {
    if (tokens == 0 || calibration == 1.0) {
        return tokens;
    }
    const double scaled = static_cast<double>(tokens) * calibration;
    const std::size_t rounded = static_cast<std::size_t>(scaled + 0.5);
    return rounded == 0 ? 1 : rounded;
}

std::size_t EstimateUtf8Tokens(const std::string& text, double calibration) {
    std::size_t ascii = 0;
    std::size_t codepoints = 0;  // 全部码点;非 ASCII 数 = codepoints - ascii
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if ((byte & 0xC0) != 0x80) {
            ++codepoints;  // UTF-8 后续字节不算新码点
        }
        if (byte < 0x80) {
            ++ascii;
        }
    }
    const std::size_t non_ascii = codepoints >= ascii ? codepoints - ascii : 0;
    // ASCII 4 字符 1 token;非 ASCII 3 token 折 2 字。不引分词器,预算够用。
    // 校准系数只在总量上落一次(内部各段按默认尺累加),免得逐段四舍
    // 五入把误差叠起来。
    return ApplyTokenCalibration(ascii / 4 + (non_ascii * 3) / 2, calibration);
}

std::size_t EstimateMessageTokens(const api::Message& message, double calibration) {
    std::size_t total = 0;
    for (const auto& block : message.content) {
        total += std::visit(
            [](const auto& b) -> std::size_t {
                using T = std::decay_t<decltype(b)>;
                if constexpr (std::is_same_v<T, api::TextBlock>) {
                    return EstimateUtf8Tokens(b.text);
                } else if constexpr (std::is_same_v<T, api::ImageBlock>) {
                    // 图片按像素折 token(宽×高/750):与预检、工具图同一条
                    // 公共尺(EstimateImageTokensForPreflight)——预算、压缩
                    // 决策与窗口预检三处口径要一致,不然贴一张大图就把
                    // /context 显示与自动压缩一起带偏。读不出宽高退字节口径。
                    return EstimateImageTokensForPreflight(b.width, b.height, b.data);
                } else if constexpr (std::is_same_v<T, api::ToolUseBlock>) {
                    return EstimateUtf8Tokens(b.name) + EstimateUtf8Tokens(b.id) +
                           EstimateUtf8Tokens(b.input.dump());
                } else if constexpr (std::is_same_v<T, api::ToolResultBlock>) {
                    // 工具结果图片回喂单:重灌过的 wire_base64 会真上 wire,
                    // token 与用户贴图 ImageBlock 同走像素口径公共尺;durable
                    // history 里 wire_base64 恒空(0 账),老行为不变。
                    std::size_t image_tokens = 0;
                    for (const auto& rich : b.blocks) {
                        if (const auto* image = std::get_if<tools::ImageContent>(&rich);
                            image != nullptr) {
                            image_tokens += EstimateImageTokensForPreflight(image->width, image->height,
                                                                           image->wire_base64);
                        }
                    }
                    return EstimateUtf8Tokens(b.tool_use_id) + EstimateUtf8Tokens(b.content) +
                           image_tokens;
                } else if constexpr (std::is_same_v<T, api::ThinkingBlock>) {
                    return EstimateUtf8Tokens(b.text) + EstimateUtf8Tokens(b.signature);
                } else {
                    return 0;
                }
            },
            block);
    }
    return ApplyTokenCalibration(total, calibration);
}

std::size_t EstimateHistoryTokens(const std::vector<api::Message>& history, double calibration) {
    std::size_t total = 0;
    for (const auto& message : history) {
        total += EstimateMessageTokens(message);
    }
    return ApplyTokenCalibration(total, calibration);
}

bool IsUserTurnStart(const api::Message& message) {
    if (message.role != api::Role::User) {
        return false;
    }
    for (const auto& block : message.content) {
        if (std::holds_alternative<api::TextBlock>(block) || std::holds_alternative<api::ImageBlock>(block)) {
            return true;
        }
    }
    return false;
}

std::vector<std::pair<std::size_t, std::size_t>> SplitIntoTurns(const std::vector<api::Message>& history) {
    std::vector<std::pair<std::size_t, std::size_t>> turns;
    for (std::size_t i = 0; i < history.size(); ++i) {
        if (IsUserTurnStart(history[i])) {
            if (!turns.empty()) {
                turns.back().second = i;
            }
            turns.emplace_back(i, history.size());
        }
    }
    return turns;
}

void InjectIncomingMessage(std::vector<api::Message>& history, api::Message incoming) {
    if (incoming.role != api::Role::User || incoming.content.empty()) {
        return;
    }
    if (!history.empty() && history.back().role == api::Role::User) {
        // 末条是 user(最常见:刚攒完的 tool_result 消息)——文本块追加进
        // 去即可,不起第二条连排的 user 消息,三种 wire 协议都安全。
        for (auto& block : incoming.content) {
            history.back().content.push_back(std::move(block));
        }
        return;
    }
    history.push_back(std::move(incoming));
}

}  // namespace lubancode::agent
