// 渐进式上下文仓(第二期:可追回 artifact)。
//
// 现状里 `[artifact eN ...]` 只是请求视图里的一行字样,模型不能按它搜全
// 文、不能读某段。这一层把超长工具结果的真本落成内容寻址仓:
//
//   <sessions_dir>/<session-id>/context/
//     index.jsonl          每枚 artifact 一行(ArtifactRef 全字段)
//     blobs/<sha256>.txt   全文真本,内容寻址(同内容只存一份)
//     chunks/<sha256>.json 分块索引(稳定 chunk_id、行号、字节范围、局部 hash)
//
// 硬规矩(规格"产品不变量"与"渐进式上下文仓"):
//   - 新长结果先原子写 blob(tmp + rename),再写 chunks 索引,最后才把
//     请求视图换成引用;任一步失败返回 nullopt,调用方保留内存正文照旧
//     发送——磁盘失败绝不把全文换成空引用。
//   - session JSONL 照旧存全量真本,仓只是加一份可检索的索引(第五期才
//     谈存档瘦身)。/resume、/export 不依赖仓也可用。
//   - 分块不劈 UTF-8 码点:所有边界都落在行首(行本身是完整序列),字节
//     范围 [byte_start, byte_end) 与行号可互查。
//   - 检索只做行级关键词(ASCII 大小写不敏感),不绑向量库(规格"索引先
//     轻后重");索引可从 blob 重建,blob 才是真本。
//   - scope 只认稳定 artifact_id:工具不吃磁盘路径,别的会话、别的目录
//     一概查不到(当前会话 + 同仓共享的子代理任务,规格"两把只读钥匙")。
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::agent {

// 内容类型:分块边界按它选(规格"分块不能一刀切字节")。
enum class ArtifactContentKind { Text, Markdown, SourceCode, Json, CommandLog };

// 按内容与工具名猜类型(纯函数):run_command 输出算 CommandLog;以 {
// 或 [ 起头且解析得动算 Json;有 Markdown 标题行算 Markdown;其余按 Text
// (源码不靠猜——分块边界与 Text 同为行窗,只是标题栏写清 guessed)。
ArtifactContentKind DetectArtifactKind(const std::string& tool_name, const std::string& content);

// 一枚 artifact 的登记行(index.jsonl 一行)。
struct ArtifactRef {
    std::string artifact_id;           // "a0007":会话内递增,稳定
    std::string session_id;
    std::string tool_use_id;
    std::string tool_name;
    std::string mime;                  // text/plain; charset=utf-8 等
    std::string encoding = "utf-8";
    std::size_t bytes = 0;
    std::size_t lines = 0;
    std::string sha256;                // 64 位十六进制小写
    std::string created_at;            // "yyyy-mm-dd HH:MM:SS"
    std::size_t source_message_index = 0;
    std::string preview;               // 头部一行(索引可读性,非请求视图)
    std::string blob_path;             // "blobs/<sha>.txt"(相对仓根)
    std::string chunk_index_path;      // "chunks/<sha>.json"(相对仓根)

    nlohmann::json ToJson() const;
    static std::optional<ArtifactRef> FromJson(const nlohmann::json& json);
};

// 一枚分块:稳定 chunk_id("c000" 起)、1 起行号、字节范围、局部指纹。
// heading 非空时是本块的语义标题(Markdown 标题 / 源码符号行 / JSON 路径)。
struct ArtifactChunk {
    std::string chunk_id;
    std::size_t line_start = 1;   // 1 起(模型与人都从 1 数行)
    std::size_t line_count = 0;
    std::size_t byte_start = 0;
    std::size_t byte_end = 0;     // [start, end)
    std::string hash;             // 块正文的 FNV-1a 64 指纹
    std::string heading;

    nlohmann::json ToJson() const;
    static std::optional<ArtifactChunk> FromJson(const nlohmann::json& json);
};

// 分块(纯函数,单测钉边界):按内容类型选边界——Markdown 优先标题、
// 源码按大括号深度归零、JSON 按顶层结构行、日志按行窗;一律不劈 UTF-8
// 码点(边界只落行首)。target_bytes 是块的目标大小(默认 4096),超长段
// 落在行边界硬切。返回块表(空内容给空表)。
std::vector<ArtifactChunk> ChunkArtifact(const std::string& content, ArtifactContentKind kind,
                                         std::size_t target_bytes = 4096);

// 行级检索(纯函数):ASCII 大小写不敏感的子串匹配,中文按原文子串;每
// 命中一行给一行(line 1 起)+ 小片预览(命中处前后各扩一点,不劈码点)。
// 按命中次数排序,同分行号升序。max_results 封顶。
struct ArtifactSearchHit {
    std::size_t line = 0;      // 1 起
    std::string chunk_id;      // 命中行落在哪块(可空 = 没给块表)
    std::string snippet;
    int score = 0;             // 该行命中次数
};
std::vector<ArtifactSearchHit> SearchArtifactContent(const std::string& content,
                                                     const std::vector<ArtifactChunk>& chunks,
                                                     const std::string& query, int max_results = 8);

// 一场会话的上下文仓。未 Open 前 active() 为假,一切操作安全退化为
// "没有 artifact"(调用方保留内存全文)。
class ContextArtifactStore {
public:
    ContextArtifactStore() = default;
    // 开仓:建 <root>(含 blobs/、chunks/),读回已有 index.jsonl(断点续
    // 卸:resume 后同会话接着编号)。目录建不成或索引读不动都只算开仓
    // 失败——active() 保持假,不拦会话。
    bool Open(std::string root_dir, std::string session_id);
    bool active() const { return active_; }
    // /clear 换场:关仓清内存索引(磁盘上的真本不动)。工具们共享同一只
    // 仓对象,Reset 后自然查不到旧会话的 artifact——scope 只跟当前会话。
    void Close() {
        active_ = false;
        refs_.clear();
        next_seq_ = 1;
        root_.clear();
        session_id_.clear();
    }
    const std::string& root() const { return root_; }
    const std::string& session_id() const { return session_id_; }

    // 卸载一枚长结果(幂等:同 tool_use_id 已在仓里,直接还旧 ref,不重写
    // blob 也不追加索引行)。原子次序见文件头。content 为空或仓未开给
    // nullopt。blob 写失败/索引追加失败都给 nullopt——内存全文还在调用方
    // 手里,不许生成假引用。
    std::optional<ArtifactRef> Offload(const std::string& tool_use_id, const std::string& tool_name,
                                       const std::string& content, std::size_t source_message_index);

    // 稳定 id 查找(工具的 scope 边界:只认这个口子,不认磁盘路径)。
    const ArtifactRef* Find(const std::string& artifact_id) const;

    // 读 blob 全文并核 sha256:不合(被改/被截)给 nullopt 并在 error 里
    // 说明,调用方拒绝供给——hash 不合立即隔离(规格"失败与安全")。
    std::optional<std::string> ReadBlobVerified(const ArtifactRef& ref, std::string* error = nullptr) const;

    // 分块索引(ref 对应那份;已缓存直接还,没缓存从 chunks/<sha>.json 读
    // 并核 hash;读不动给空表,调用方退化为按行读)。
    std::vector<ArtifactChunk> ChunksFor(const ArtifactRef& ref) const;

    struct ReadResult {
        bool ok = false;
        std::string error;         // !ok 时的人话
        std::size_t line_start = 0;
        std::size_t line_count = 0;
        std::string chunk_id;
        std::string text;          // ok 时的正文(不带行号,头部信息在字段里)
        std::string available;     // !ok 时给可用范围("可用行 1-396,块 c000-c003")
    };
    // 读一段:chunk_id 非空按块读(行窗参数忽略);否则 line_start(1 起)
    // + line_count。line_count 为 0 = 读到结尾。max_bytes 是 token 预算的
    // 字节面(默认 32 KiB):超了拒绝并给可用范围,不悄悄截(规格"读太大
    // 便拒绝,并给可用范围")。
    ReadResult Read(const ArtifactRef& ref, const std::string& chunk_id, std::size_t line_start,
                    std::size_t line_count, std::size_t max_bytes = 32 * 1024) const;

    // 检索仓内一枚 artifact(ReadBlobVerified 同一道 hash 门)。
    std::optional<std::vector<ArtifactSearchHit>> Search(const ArtifactRef& ref, const std::string& query,
                                                         int max_results, std::string* error = nullptr) const;

    struct Stats {
        std::size_t artifacts = 0;
        std::size_t total_bytes = 0;   // 全部 blob 的字节合计(同一 blob 只按一次)
    };
    Stats StatsOf() const;

    const std::vector<ArtifactRef>& refs() const { return refs_; }

private:
    bool active_ = false;
    std::string root_;
    std::string session_id_;
    std::vector<ArtifactRef> refs_;
    std::size_t next_seq_ = 1;  // artifact_id 编号(a0001 起)
};

}  // namespace lubancode::agent
