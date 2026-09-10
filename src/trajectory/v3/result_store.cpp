// 工具结果仓与模型预览实现(§4.16-4.17)。
#include "trajectory/v3/result_store.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <set>

#include "hooks/hash.hpp"

namespace lubancode::trajectory::v3 {

namespace {

// ---------------------------------------------------------------------------
// UTF-8 边界工具(§4.17:按字节帽截断,不切出非法 UTF-8)
// ---------------------------------------------------------------------------

bool AtCharBoundary(std::string_view text, std::size_t position) {
    if (position == 0 || position >= text.size()) {
        return true;
    }
    return (static_cast<unsigned char>(text[position]) & 0xC0) != 0x80;
}

std::size_t AlignBack(std::string_view text, std::size_t cut) {
    while (cut > 0 && !AtCharBoundary(text, cut)) {
        --cut;
    }
    return cut;
}

std::size_t AlignForward(std::string_view text, std::size_t cut) {
    while (cut < text.size() && !AtCharBoundary(text, cut)) {
        ++cut;
    }
    return cut;
}

std::string Marker(const PreviewChannel& channel) {
    return "[文件: " + channel.display_path + " | 通道: " + channel.channel + "]";
}

std::string FormatBytes(std::uint64_t bytes, bool lower_bound) {
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(bytes));
    return lower_bound ? ">= " + std::string(buffer) : std::string(buffer);
}

// ---------------------------------------------------------------------------
// 预览内部:通道正文段(head/tail 切进通道中间时逐段标来源,§4.17)
// ---------------------------------------------------------------------------

struct ChannelSpan {
    std::size_t begin = 0;
    std::size_t end = 0;  // [begin, end) 在拼接正文里的区间
};

struct Segment {
    std::size_t channel_index = 0;
    std::string text;
};

// 正文 = 各通道文本按稳定次序拼接;记录每通道区间。
std::string JoinBodies(const std::vector<PreviewChannel>& channels,
                       std::vector<ChannelSpan>* spans) {
    std::string body;
    spans->clear();
    for (const auto& channel : channels) {
        ChannelSpan span;
        span.begin = body.size();
        body += channel.text;
        span.end = body.size();
        spans->push_back(span);
    }
    return body;
}

// 把 [begin,end) 区间按通道边界切段;每段自带所属通道。
std::vector<Segment> SliceSpan(const std::string& body,
                               const std::vector<ChannelSpan>& spans,
                               const std::vector<PreviewChannel>& channels, std::size_t begin,
                               std::size_t end) {
    std::vector<Segment> segments;
    for (std::size_t i = 0; i < spans.size(); ++i) {
        const std::size_t from = std::max(begin, spans[i].begin);
        const std::size_t to = std::min(end, spans[i].end);
        if (from >= to) {
            continue;
        }
        // 空文本通道不产段;来源标记只为有正文的段出现。
        if (channels[i].text.empty()) {
            continue;
        }
        segments.push_back(Segment{i, body.substr(from, to - from)});
    }
    return segments;
}

// 渲染一段:来源标记 + 正文(尾段从文件中间开始也重标来源,§4.17)。
void RenderSegments(const std::vector<PreviewChannel>& channels,
                    const std::vector<Segment>& segments, std::string* out) {
    for (const auto& segment : segments) {
        out->append(Marker(channels[segment.channel_index]));
        out->append("\n");
        out->append(segment.text);
        out->append("\n");
    }
}

std::string JoinPaths(const std::vector<std::string>& paths) {
    std::string out = "[";
    for (std::size_t i = 0; i < paths.size(); ++i) {
        if (i > 0) {
            out += ", ";
        }
        out += "\"" + paths[i] + "\"";
    }
    out += "]";
    return out;
}

}  // namespace

PreviewResult BuildToolPreview(const PreviewRequest& request) {
    PreviewResult result;
    const std::uint64_t budget = request.max_preview_bytes;
    const auto& channels = request.channels;

    // 1. 完整性归类(§4.17):full_output 列收全材料;部分材料列
    //    captured_output;全部不完整时 full_output 为 []。
    for (const auto& channel : channels) {
        if (channel.capture_complete) {
            result.full_output.push_back(channel.display_path);
        } else {
            result.captured_output.push_back(channel.display_path);
        }
    }
    if (!channels.empty() && result.captured_output.size() == channels.size()) {
        result.full_output.clear();
    }

    // 2. 说明区渲染。清单装不下时:有 output_index 就缩清单,没有就标记
    //    listing_overflow 让调用方先存清单再重算(§4.17 极端情况)。
    auto render_header = [&](bool truncated, std::size_t listing_cap) {
        std::string header;
        if (request.exit_code.has_value()) {
            header += "exit_code: " + std::to_string(*request.exit_code) + "\n";
        }
        for (const auto& channel : channels) {
            if (channel.output_bytes == 0 && channel.text.empty()) {
                continue;  // 没有该通道与通道为空分清:零输出不虚列
            }
            header += "output_bytes[" + channel.channel +
                      "]: " + FormatBytes(channel.output_bytes, channel.output_bytes_lower_bound) +
                      "\n";
        }
        header += std::string("truncated: ") + (truncated ? "true" : "false") + "\n";
        bool all_complete = result.captured_output.empty();
        header += std::string("capture_complete: ") + (all_complete ? "true" : "false") + "\n";
        for (const auto& channel : channels) {
            if (!channel.capture_complete) {
                header += "capture[" + channel.channel + "]: " +
                          (channel.capture_reason.empty() ? std::string("incomplete")
                                                          : channel.capture_reason) +
                          "\n";
            }
        }
        // 清单:预算内全列;超限缩到容纳得下的前几条 + 省略计数。
        std::size_t shown = result.full_output.size();
        if (listing_cap < shown) {
            shown = listing_cap;
        }
        std::vector<std::string> listed(result.full_output.begin(),
                                        result.full_output.begin() + static_cast<std::ptrdiff_t>(shown));
        header += "full_output: " + JoinPaths(listed) + "\n";
        if (!result.captured_output.empty()) {
            header += "captured_output: " + JoinPaths(result.captured_output) + "\n";
        }
        if (shown < result.full_output.size()) {
            if (request.output_index_path.has_value()) {
                result.output_index_path = request.output_index_path;
                header += "output_index: " + *request.output_index_path + "\n";
            }
            result.omitted_output_count =
                static_cast<int>(result.full_output.size() - shown);
            header += "omitted_output_count: " + std::to_string(result.omitted_output_count) + "\n";
        }
        return header;
    };

    // 先按全清单渲染;若说明区+骨架已顶到预算,进入缩清单/溢出路径。
    std::size_t listing_cap = result.full_output.size();
    const std::size_t full_header_len = render_header(false, listing_cap).size();
    std::size_t skeleton = full_header_len + std::string("\n[中间内容已省略]\n").size() +
                           std::string("[开头内容]\n").size() + std::string("[结尾内容]\n").size();
    if (static_cast<std::uint64_t>(skeleton) >= budget) {
        // 清单自身超限(§4.17)。
        result.listing_text = render_header(false, listing_cap);
        if (!request.output_index_path.has_value()) {
            result.listing_overflow = true;
            result.truncated = true;
            // 交代得了多少算多少:只留状态与溢出说明,不硬塞清单。
            std::string minimal = "truncated: true\ncapture_complete: " +
                                  std::string(result.captured_output.empty() ? "true" : "false") +
                                  "\nfull_output: [清单超限,待 output_index]\n";
            result.text = minimal;
            if (minimal.size() > budget) {
                result.text = minimal.substr(0, AlignBack(minimal, static_cast<std::size_t>(budget)));
                result.preview_unrepresentable = true;
            }
            return result;
        }
        // 有 output_index:缩清单到装得下为止(先保状态与入口,再保条目)。
        while (listing_cap > 0) {
            std::size_t len = render_header(true, listing_cap).size() +
                              std::string("\n[中间内容已省略]\n").size();
            if (static_cast<std::uint64_t>(len) < budget) {
                break;
            }
            --listing_cap;
        }
        if (listing_cap == 0) {
            // 必要来源也装不下(§4.38 preview_unrepresentable)。
            result.preview_unrepresentable = true;
            std::string minimal = "truncated: true\noutput_index: " + *request.output_index_path +
                                  "\nomitted_output_count: " +
                                  std::to_string(result.full_output.size()) + "\n";
            result.text = minimal.size() > budget
                              ? minimal.substr(0, AlignBack(minimal, static_cast<std::size_t>(budget)))
                              : minimal;
            result.truncated = true;
            return result;
        }
    }

    // 3. 完整渲染(不截断)装得下就原样返回(§4.17)。
    {
        std::string full = render_header(false, listing_cap);
        full += "\n";
        for (std::size_t i = 0; i < channels.size(); ++i) {
            if (channels[i].text.empty()) {
                continue;
            }
            full += Marker(channels[i]) + "\n" + channels[i].text + "\n";
        }
        if (static_cast<std::uint64_t>(full.size()) <= budget) {
            result.text = std::move(full);
            return result;
        }
    }

    // 4. 截断:先留说明与标记预算 M,正文 B=budget-M 头尾均分(§4.17)。
    result.truncated = true;
    std::vector<ChannelSpan> spans;
    const std::string body = JoinBodies(channels, &spans);
    std::size_t head_len = 0;
    std::size_t tail_len = 0;
    for (int round = 0; round < 4; ++round) {
        // 渲染一次拿真实 M(标记/提示/换行都算),再定 B;首轮 head/tail=0
        // 只探固定成本,不据此提前收笔。
        std::vector<Segment> head_segments = SliceSpan(body, spans, channels, 0, head_len);
        std::vector<Segment> tail_segments =
            SliceSpan(body, spans, channels, body.size() - std::min(tail_len, body.size()),
                      body.size());
        std::string probe = render_header(true, listing_cap);
        probe += "\n[开头内容]\n";
        RenderSegments(channels, head_segments, &probe);
        probe += "\n[中间内容已省略]\n\n[结尾内容]\n";
        RenderSegments(channels, tail_segments, &probe);
        const std::size_t fixed = probe.size() - head_len - tail_len;
        std::size_t body_budget =
            budget > fixed ? static_cast<std::size_t>(budget - fixed) : 0;
        std::size_t new_head = body_budget / 2;
        std::size_t new_tail = body_budget - new_head;
        // 头尾不重叠(§4.17):至少留一字节给省略号位置。
        if (new_head + new_tail > body.size()) {
            const std::size_t room = body.size() > 0 ? body.size() - 1 : 0;
            new_head = std::min(new_head, room / 2);
            new_tail = std::min(new_tail, room - new_head);
        }
        if (new_head == head_len && new_tail == tail_len) {
            break;  // 收敛
        }
        head_len = new_head;
        tail_len = new_tail;
    }
    // 5. 终稿:边界对齐、总长复核,超了先削尾再削头(不截坏标记)。
    auto render_final = [&]() {
        std::string out = render_header(true, listing_cap);
        out += "\n[开头内容]\n";
        std::vector<Segment> head_segments = SliceSpan(
            body, spans, channels, 0, AlignBack(body, head_len));
        RenderSegments(channels, head_segments, &out);
        out += "\n[中间内容已省略]\n\n[结尾内容]\n";
        const std::size_t tail_begin = AlignForward(body, body.size() - std::min(tail_len, body.size()));
        std::vector<Segment> tail_segments = SliceSpan(body, spans, channels, tail_begin, body.size());
        RenderSegments(channels, tail_segments, &out);
        // 没展示正文的通道点名(§4.17:未展示正文与文件缺失分清)。
        if (!head_segments.empty() || !tail_segments.empty()) {
            std::vector<std::string> not_shown;
            for (std::size_t i = 0; i < channels.size(); ++i) {
                if (channels[i].text.empty()) {
                    continue;
                }
                bool shown = false;
                for (const auto& segment : head_segments) {
                    shown = shown || segment.channel_index == i;
                }
                for (const auto& segment : tail_segments) {
                    shown = shown || segment.channel_index == i;
                }
                if (!shown) {
                    not_shown.push_back(channels[i].display_path);
                }
            }
            if (!not_shown.empty()) {
                out += "not_shown: " + JoinPaths(not_shown) + "\n";
            }
        }
        return out;
    };
    std::string text = render_final();
    while (text.size() > budget && tail_len > 0) {
        tail_len = AlignBack(body, tail_len - 1);
        text = render_final();
    }
    while (text.size() > budget && head_len > 0) {
        head_len = AlignBack(body, head_len - 1);
        text = render_final();
    }
    if (text.size() > budget) {
        // 连说明区都装不下:不可表示(§4.38),按边界硬收,明报。
        result.preview_unrepresentable = true;
        text = text.substr(0, AlignBack(text, static_cast<std::size_t>(budget)));
    }
    result.text = std::move(text);
    return result;
}

// ---------------------------------------------------------------------------
// 结果仓
// ---------------------------------------------------------------------------

namespace {

// 落稳次序(§4.16):临时文件 -> 落稳 -> 改不可变名。不可变名撞车
// (POSIX rename 会静默覆盖)先显式查存在性:已存在即冲突,不覆盖。
bool WriteImmutable(const std::filesystem::path& final_path, const std::string& data,
                    std::string* error) {
    std::error_code ec;
    if (std::filesystem::exists(final_path, ec)) {
        *error = "结果仓不可变名已存在(结果不许覆盖): " + final_path.string();
        return false;
    }
    std::filesystem::path temp = final_path;
    temp += ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            *error = "结果仓开不了临时文件: " + temp.string();
            return false;
        }
        file.write(data.data(), static_cast<std::streamsize>(data.size()));
        file.flush();
        if (!file.good()) {
            *error = "结果仓写临时文件失败: " + temp.string();
            return false;
        }
    }
    std::filesystem::rename(temp, final_path, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        *error = "结果仓发布不可变名失败: " + final_path.string();
        return false;
    }
    return true;
}

std::string ExtensionFor(const std::string& media_type) {
    if (media_type == "application/json") {
        return "json";
    }
    if (media_type.rfind("text/", 0) == 0) {
        return "txt";
    }
    return "bin";
}

std::string ZeroPad6(std::uint64_t value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%06llu", static_cast<unsigned long long>(value));
    return buffer;
}

}  // namespace

nlohmann::json MakeArtifactRef(std::string artifact_id, std::string kind, std::string path,
                               std::string sha256, std::uint64_t bytes, std::string media_type) {
    return nlohmann::json::object({{"artifactId", std::move(artifact_id)},
                                   {"kind", std::move(kind)},
                                   {"path", std::move(path)},
                                   {"sha256", std::move(sha256)},
                                   {"bytes", bytes},
                                   {"mediaType", std::move(media_type)}});
}

std::expected<ResultStore, std::string> ResultStore::Open(
    const std::filesystem::path& session_dir) {
    std::filesystem::path artifacts = session_dir / "artifacts";
    std::error_code ec;
    std::filesystem::create_directories(artifacts, ec);
    if (ec) {
        return std::unexpected("结果仓建目录失败: " + artifacts.string());
    }
    std::uint64_t next = 1;
    for (const auto& entry : std::filesystem::directory_iterator(artifacts, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("res-", 0) != 0) {
            continue;
        }
        auto dot = name.find('.');
        std::string number =
            name.substr(4, dot == std::string::npos ? std::string::npos : dot - 4);
        if (number.empty() ||
            !std::all_of(number.begin(), number.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            continue;
        }
        std::uint64_t value = std::stoull(number);
        if (value >= next) {
            next = value + 1;
        }
    }
    return ResultStore(std::move(artifacts), next);
}

ResultStore::ResultStore(std::filesystem::path artifacts_dir, std::uint64_t next_result_number)
    : artifacts_dir_(std::move(artifacts_dir)), next_result_number_(next_result_number) {}

ResultStore::PersistedResult ResultStore::Persist(const PersistRequest& request) {
    PersistedResult outcome;
    const std::string result_id = "res-" + ZeroPad6(next_result_number_);
    nlohmann::json outputs = nlohmann::json::array();
    std::vector<nlohmann::json> result_ref;
    for (const auto& output : request.outputs) {
        nlohmann::json entry = nlohmann::json::object();
        if (!output.data.empty()) {
            const std::string extension = ExtensionFor(output.media_type);
            const std::string file_name =
                result_id + "." + output.channel + "." + extension;
            const std::filesystem::path final_path = artifacts_dir_ / file_name;
            std::string error;
            if (!WriteImmutable(final_path, output.data, &error)) {
                outcome.error = error;
                return outcome;
            }
            const std::string sha = hooks::Sha256Hex(output.data);
            entry["ref"] = nlohmann::json::object(
                {{"artifact_id", result_id + "-" + output.channel},
                 {"path", "artifacts/" + file_name},
                 {"sha256", sha},
                 {"bytes", output.data.size()},
                 {"media_type", output.media_type}});
            entry["captured_bytes"] = output.data.size();
            result_ref.push_back(MakeArtifactRef(result_id + "-" + output.channel,
                                                 output.channel, "artifacts/" + file_name, sha,
                                                 output.data.size(), output.media_type));
        }
        // 空通道且收全:不造空文件,描述里记 bytes=0(§4.16);
        // 没有该通道 = outputs 不列,与空捕获分清。
        entry["output_bytes"] = output.output_bytes;
        entry["byte_count_kind"] =
            output.output_bytes_lower_bound ? "lower_bound" : "exact";
        entry["capture_complete"] = output.capture_complete;
        if (!output.capture_complete) {
            entry["capture_reason"] = output.capture_reason;
        }
        entry["encoding"] = output.encoding;
        outputs.push_back(std::move(entry));
    }
    // 不可变描述文件(§4.16 字段表;snake_case——工具协议字段)。
    nlohmann::json metadata = nlohmann::json::object(
        {{"result_id", result_id},
         {"tool_call_id", request.tool_call_id},
         {"attempt", request.attempt},
         {"result_kind", request.result_kind},
         {"execution_event_ref", request.execution_event_ref},
         {"preview_policy", request.preview_policy},
         {"capture_limits", request.capture_limits},
         {"outputs", std::move(outputs)}});
    if (!request.content.is_null()) {
        metadata["content"] = request.content;
    }
    if (!request.structured_content.is_null()) {
        metadata["structured_content"] = request.structured_content;
    }
    const std::string metadata_text = metadata.dump(2);
    const std::string metadata_name = result_id + ".json";
    std::string error;
    if (!WriteImmutable(artifacts_dir_ / metadata_name, metadata_text, &error)) {
        outcome.error = error;
        return outcome;
    }
    result_ref.insert(
        result_ref.begin(),
        MakeArtifactRef(result_id, "result_metadata", "artifacts/" + metadata_name,
                        hooks::Sha256Hex(metadata_text), metadata_text.size(),
                        "application/json"));
    ++next_result_number_;
    outcome.result_id = result_id;
    outcome.result_ref = std::move(result_ref);
    outcome.ok = true;
    return outcome;
}

std::expected<std::string, std::string> ResultStore::PersistListing(const std::string& listing_name,
                                                                    const std::string& text) {
    const std::filesystem::path final_path = artifacts_dir_ / listing_name;
    std::string error;
    if (!WriteImmutable(final_path, text, &error)) {
        return std::unexpected(error);
    }
    return "artifacts/" + listing_name;
}

}  // namespace lubancode::trajectory::v3
