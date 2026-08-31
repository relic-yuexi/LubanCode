// search 内置 ripgrep 后端迁移单(P0-1)的旧内核照相机,P0-5 起兼任差分器。
//
// 不进 ctest,手动跑:
//   search_golden_driver golden  <输出 JSON 路径>
//       把 tests/fixtures/search/corpus/ 这批夹具逐条场景喂给 SearchTool::
//       execute(),按"路径集合/命中行集合"归一化后落盘。P0-1 用它给旧内核
//      (std::regex + recursive_directory_iterator)拍照;旧内核删掉后,它
//       记录的是"当前后端"的快照——旧基线冻结在
//       tests/fixtures/search/golden/old_kernel_golden.json,不再重造。
//   search_golden_driver diff <基线 JSON> <rg 可执行路径> [输出 JSON]
//       P0-5 的差分主口:同一份场景表、同一套归一化,经注入的随包 rg 跑
//       一遍,与基线逐条对账。"必须保留"全等;"有意迁移"只准落在批准表
//       里(ignore 语义、ECMAScript 独有语法、16 KiB 长行截断、非法 UTF-8
//       清洗——见 docs/development/search-ripgrep-migration.md 的分栏表)。
//       全部过门 exit 0;任何未批准的出入 exit 1 并把明细写到输出 JSON。
//   search_golden_driver bench   <输出 JSON 路径> [语料目录=src]
//       对当前后端跑一组基准查询(纯字面量/正则/无命中/高频命中/glob),
//       每档跑若干轮取 P50/P95,连同遍历文件数、耗时一并落盘。默认语料
//       是本仓 src/ 目录(medium 档);可传别的目录路径覆盖。
//
// 子命令共享同一份"调用 SearchTool、量时钟"的骨架,分开落盘方便报告分别
// 引用。
//
// golden JSON 格式(数组,每个场景一个对象):
//   {
//     "id": "grep_chinese",                 场景名,差分表按它对齐
//     "mode": "grep" | "glob",
//     "pattern": "...",
//     "path": "chinese",                    相对 corpus/ 的路径,跨机器可移植
//     "glob": "",                           mode=grep 时的可选 glob 过滤
//     "is_error": false,
//     "hit_count": 1,
//     "truncated": false,
//     "notice_present": false,              是否带观察边界提示(不存路径本身)
//     "notice_over_threshold": false,
//     "hits_sorted": ["chinese/chinese.txt:2:这里有关键词chinese_needle"],
//     "hits_sha256": "..."                  sorted hits 拼接后的摘要,便于快速比对
//   }
// 比较时按 hits_sorted 集合与 hit_count/is_error/truncated 等标量对比,
// 不比较 SearchTool 原始输出里的行序——旧内核的 recursive_directory_
// iterator 顺序本就不是合同的一部分,ripgrep 的并行 walker 顺序更不会
// 与之相同。

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "tools/path_utils.hpp"
#include "tools/search.hpp"
#include "tools/search_ripgrep.hpp"


using lubancode::tools::PathToUtf8;
using lubancode::tools::SearchTool;
using lubancode::tools::Tool;
using lubancode::tools::Utf8ToPath;

namespace {

namespace fs = std::filesystem;

// ---- sha256(纯 C++,够用即可,不引第三方库) --------------------------
// 教科书实现,不追求速度,只用来给 golden 记录生成一枚短摘要方便肉眼比对。
class Sha256 {
public:
    Sha256() { Reset(); }

    void Update(const std::string& data) {
        for (unsigned char c : data) {
            buffer_[buffer_len_++] = c;
            if (buffer_len_ == 64) {
                Transform(buffer_.data());
                bit_len_ += 512;
                buffer_len_ = 0;
            }
        }
    }

    std::string HexDigest() {
        std::uint64_t total_bits = bit_len_ + static_cast<std::uint64_t>(buffer_len_) * 8;
        buffer_[buffer_len_++] = 0x80;
        if (buffer_len_ > 56) {
            while (buffer_len_ < 64) buffer_[buffer_len_++] = 0;
            Transform(buffer_.data());
            buffer_len_ = 0;
        }
        while (buffer_len_ < 56) buffer_[buffer_len_++] = 0;
        for (int i = 7; i >= 0; --i) {
            buffer_[buffer_len_++] = static_cast<unsigned char>((total_bits >> (i * 8)) & 0xff);
        }
        Transform(buffer_.data());

        std::ostringstream out;
        for (std::uint32_t h : state_) {
            char hex[9];
            std::snprintf(hex, sizeof(hex), "%08x", h);
            out << hex;
        }
        return out.str();
    }

private:
    void Reset() {
        state_ = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                  0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
        bit_len_ = 0;
        buffer_len_ = 0;
    }

    static std::uint32_t RotR(std::uint32_t x, std::uint32_t n) { return (x >> n) | (x << (32 - n)); }

    void Transform(const unsigned char* chunk) {
        static const std::uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(chunk[i * 4]) << 24) |
                   (static_cast<std::uint32_t>(chunk[i * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(chunk[i * 4 + 2]) << 8) |
                   static_cast<std::uint32_t>(chunk[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            std::uint32_t s0 = RotR(w[i - 15], 7) ^ RotR(w[i - 15], 18) ^ (w[i - 15] >> 3);
            std::uint32_t s1 = RotR(w[i - 2], 17) ^ RotR(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (int i = 0; i < 64; ++i) {
            std::uint32_t s1 = RotR(e, 6) ^ RotR(e, 11) ^ RotR(e, 25);
            std::uint32_t ch = (e & f) ^ ((~e) & g);
            std::uint32_t temp1 = h + s1 + ch + k[i] + w[i];
            std::uint32_t s0 = RotR(a, 2) ^ RotR(a, 13) ^ RotR(a, 22);
            std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            std::uint32_t temp2 = s0 + maj;
            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_;
    std::uint64_t bit_len_ = 0;
    std::array<unsigned char, 64> buffer_{};
    std::size_t buffer_len_ = 0;
};

std::string Sha256Hex(const std::string& data) {
    Sha256 h;
    h.Update(data);
    return h.HexDigest();
}

// ---- 场景表 ------------------------------------------------------------

struct Scenario {
    std::string id;
    std::string mode;
    std::string pattern;
    std::string rel_path;  // 相对 corpus/ 根
    std::string glob;      // 仅 grep 用,可空
};

std::vector<Scenario> BuildScenarios() {
    return {
        {"grep_chinese", "grep", "关键词", "chinese", ""},
        {"grep_crlf", "grep", "needle_crlf", "crlf", ""},
        {"grep_empty_no_hit", "grep", "anything_at_all", "empty", ""},
        {"grep_binary_skipped", "grep", "needle_bin", "binary", ""},
        {"grep_long_line_anchor", "grep", "needle_long_line_anchor", "long_line", ""},
        {"grep_many_hits_truncated", "grep", "hit_line_150", "many_hits", ""},
        {"grep_hidden_included", "grep", "needle", "hidden", ""},
        {"grep_ignore_files_not_respected", "grep", "ignore_needle", "ignore", ""},
        {"grep_excluded_dirs_hard_skip", "grep", "excluded_needle", "excluded_dirs", ""},
        {"grep_explicit_evidence_file", "grep", "excluded_needle", "excluded_dirs/.evidence/subagent-1.log", ""},
        {"grep_explicit_evidence_dir_root", "grep", "excluded_needle", "excluded_dirs/.evidence", ""},
        {"grep_illegal_utf8_content", "grep", "utf8_needle_anchor", "illegal_utf8_content", ""},
        {"grep_glob_filter_nested_cpp", "grep", "cpp", "nested_glob", "**/*.cpp"},
        {"grep_invalid_regex", "grep", "(unclosed", "chinese", ""},
        {"grep_no_match", "grep", "zzz_definitely_absent_pattern", "chinese", ""},
        // 下两条不是"必须保留"的用例,是特意留给迁移表当证据的:pattern 用了
        // ECMAScript 有、Rust regex(默认引擎)设计上没有的语法(backreference、
        // lookahead)。旧内核(std::regex ECMAScript)应能正常编译并按此语义
        // 命中;等 P0-5 换真 rg 跑同一条,预期要么报 search_pattern_invalid,
        // 要么行为不同——这条差异正是 §6 不开 --pcre2、只用默认引擎的代价,
        // 提前留证据,不是等出了问题才补。
        {"grep_ecmascript_backreference", "grep", "(\\w+)_\\1_repeat", "regex_only_ecmascript", ""},
        {"grep_ecmascript_lookahead", "grep", "look(?=ahead_marker)", "regex_only_ecmascript", ""},

        {"glob_star_cpp_recursive", "glob", "*.cpp", "nested_glob", ""},
        {"glob_doublestar_md", "glob", "**/*.md", "nested_glob", ""},
        {"glob_docs_star_one_level", "glob", "docs/*.md", "nested_glob", ""},
        {"glob_docs_doublestar_any_depth", "glob", "docs/**", "nested_glob", ""},
        {"glob_hidden_dotfiles_included", "glob", "*.txt", "hidden", ""},
        {"glob_ignore_files_not_respected", "glob", "*.txt", "ignore", ""},
        {"glob_excluded_dirs_hard_skip", "glob", "*", "excluded_dirs", ""},
    };
}

// ---- excluded_dirs 现造(不进 git,见 tests/fixtures/search/README.md) --

void WriteFile(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << content;
}

void MaterializeExcludedDirsFixture(const fs::path& corpus_root) {
    const fs::path root = corpus_root / "excluded_dirs";
    if (fs::exists(root)) {
        return;  // 已有(比如刚跑过 build_corpus.sh),不重复生成
    }
    WriteFile(root / ".git" / "objects" / "pack_marker.txt", "excluded_needle in fake git objects\n");
    WriteFile(root / "build" / "generated.txt", "excluded_needle in build output\n");
    WriteFile(root / "node_modules" / "pkg" / "index.js", "excluded_needle in node_modules\n");
    WriteFile(root / ".evidence" / "subagent-1.log", "excluded_needle in evidence log\n");
    WriteFile(root / "real.txt", "excluded_needle in real tracked file\n");
}

// ---- 执行一条场景,归一化 --------------------------------------------

std::string NormalizeSlashes(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

nlohmann::json RunScenarioWithTool(const Scenario& sc, const fs::path& corpus_root,
                                   const std::shared_ptr<SearchTool>& tool) {
    nlohmann::json input;
    input["mode"] = sc.mode;
    input["pattern"] = sc.pattern;
    const fs::path abs_path = corpus_root / Utf8ToPath(sc.rel_path);
    input["path"] = PathToUtf8(abs_path);
    if (!sc.glob.empty()) {
        input["glob"] = sc.glob;
    }

    const Tool::Result result = tool->execute(input);

    // 观察边界提示行(若有)以 "[观察边界] " 开头,单独占一整行,后面才是
    // 真正的搜索结果正文。识别出来后从正文里摘掉,不把绝对路径存进 golden。
    std::string body = result.content;
    bool notice_present = false;
    bool notice_over_threshold = false;
    const std::string notice_prefix = "[\xe8\xa7\x82\xe5\xaf\x9f\xe8\xbe\xb9\xe7\x95\x8c] ";  // "[观察边界] "
    if (body.rfind(notice_prefix, 0) == 0) {
        const std::size_t nl = body.find('\n');
        const std::string notice_line = (nl == std::string::npos) ? body : body.substr(0, nl);
        notice_present = true;
        notice_over_threshold = notice_line.find("\xe4\xb8\x8d\xe8\xaf\xbb") != std::string::npos;  // "不读"
        body = (nl == std::string::npos) ? std::string() : body.substr(nl + 1);
    }

    std::vector<std::string> hits;
    bool truncated = body.find("\xe6\x88\xaa\xe6\x96\xad") != std::string::npos;  // "截断"
    if (!result.is_error) {
        // 无命中/无匹配是 SearchTool 自己的一句话哨兵文案,不是"一条命中",
        // 整段 body 就是这一句时直接判 0 条,不进 hits 列表——不然"没搜到"
        // 这行字本身会被误记成 hit_count=1 的假命中。
        const bool is_no_match_sentinel =
            body == "\xe6\xb2\xa1\xe6\x90\x9c\xe5\x88\xb0\xe5\x8c\xb9\xe9\x85\x8d\xe7\x9a\x84\xe5\x86\x85\xe5\xae\xb9" ||  // "没搜到匹配的内容"
            body == "\xe6\xb2\xa1\xe6\x89\xbe\xe5\x88\xb0\xe5\x8c\xb9\xe9\x85\x8d\xe7\x9a\x84\xe6\x96\x87\xe4\xbb\xb6";    // "没找到匹配的文件"
        if (!is_no_match_sentinel) {
            std::istringstream in(body);
            std::string line;
            while (std::getline(in, line)) {
                if (line.empty()) continue;
                if (line.find("\xe6\x88\xaa\xe6\x96\xad") != std::string::npos) continue;  // 截断提示行不算命中
                if (line.rfind("\xe2\x80\xa6\xe2\x80\xa6", 0) == 0) continue;  // "……" 开头的提示行
                hits.push_back(NormalizeSlashes(line));
            }
        }
    }
    std::sort(hits.begin(), hits.end());

    std::string joined;
    for (const auto& h : hits) {
        joined += h;
        joined += '\n';
    }

    nlohmann::json rec;
    rec["id"] = sc.id;
    rec["mode"] = sc.mode;
    rec["pattern"] = sc.pattern;
    rec["path"] = sc.rel_path;
    rec["glob"] = sc.glob;
    rec["is_error"] = result.is_error;
    rec["hit_count"] = hits.size();
    rec["truncated"] = truncated;
    rec["notice_present"] = notice_present;
    rec["notice_over_threshold"] = notice_over_threshold;
    rec["hits_sorted"] = hits;
    rec["hits_sha256"] = Sha256Hex(joined);
    return rec;
}

nlohmann::json RunScenario(const Scenario& sc, const fs::path& corpus_root) {
    return RunScenarioWithTool(sc, corpus_root, std::make_shared<SearchTool>());
}

int RunGolden(const fs::path& corpus_root, const fs::path& out_path) {
    MaterializeExcludedDirsFixture(corpus_root);

    nlohmann::json out = nlohmann::json::array();
    for (const Scenario& sc : BuildScenarios()) {
        out.push_back(RunScenario(sc, corpus_root));
    }

    fs::create_directories(out_path.parent_path());
    std::ofstream f(out_path, std::ios::binary);
    f << out.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) << "\n";
    std::cout << "golden 写入: " << PathToUtf8(out_path) << "  (" << out.size() << " 条场景)\n";
    return 0;
}

// ---- 差分(P0-5):当前后端 vs 旧内核冻结基线 ------------------------------

// 批准的"有意迁移"表:差分时这些场景允许与基线有出入,出入形状逐条写死;
// 不在表里的场景必须全等(is_error/truncated/hit_count/hits_sorted)。
// 依据:设计单 §5.3(ignore 语义)、§四正则迁移表、§6.5(16 KiB 长行、
// bytes 路清洗)、docs/development/search-ripgrep-migration.md 的分栏表。
struct ApprovedMigration {
    // 差分口径:
    //   ErrorOnly         两边都 is_error=true 即可(报错文案不是合同)
    //   ErrorWithCode     新侧 is_error=true 且正文含稳定码(pattern 语法)
    //   HitsSubset        新侧命中必须是旧侧命中的真子集(ignore 生效)
    //   IncludeGlobSubset 同上,但语义是"显式 -g include 压过 ignore 规则"
    //                     (rg 15.2.0 实测:被 ignore 的文件只要配得上用户
    //                      include glob 就会回来;宿主排除项是 ! 排除,不受
    //                      影响)——新命中仍必须是旧命中的子集,不许冒新文件
    //   CountOnlyCap16384 命中行数/路径前缀一致,正文截到 16 KiB(长行墙)
    //   CountOnlySanitized 命中数一致、锚点在,正文过清洗(非法 UTF-8)
    enum class Kind {
        ErrorOnly,
        ErrorWithCode,
        HitsSubset,
        IncludeGlobSubset,
        CountOnlyCap16384,
        CountOnlySanitized
    };
    const char* scenario;
    Kind kind;
    const char* expect_code;  // ErrorWithCode 用的稳定码子串
};

const std::vector<ApprovedMigration>& ApprovedMigrations() {
    static const std::vector<ApprovedMigration> table = {
        // ignore 语义迁移:旧内核不认 ignore 文件(4 命中),新内核默认遵守
        //--被 ignore 的文件不再搜,剩下的必须是旧命中的真子集。
        {"grep_ignore_files_not_respected", ApprovedMigration::Kind::HitsSubset, ""},
        // glob 模式带 `-g '*.txt'`:rg 的优先级合同是"显式 include 压过
        // ignore 规则"——被 ignore 的 .txt 配得上 include 就全回来了(实测
        // 4->4)。批准的理由:压住它只能在宿主里重长一个 ignore 引擎,那
        // 正是本单删掉的东西;与 Claude Code 的 Grep 工具同款行为。
        {"glob_ignore_files_not_respected", ApprovedMigration::Kind::IncludeGlobSubset, ""},
        // ECMAScript 独有语法:Rust regex 不认 backreference/lookahead,
        // 换成稳定错误 search_pattern_invalid(§四迁移表已预告)。
        {"grep_ecmascript_backreference", ApprovedMigration::Kind::ErrorWithCode, "search_pattern_invalid"},
        {"grep_ecmascript_lookahead", ApprovedMigration::Kind::ErrorWithCode, "search_pattern_invalid"},
        // 报错文案不是合同(旧:ECMAScript 语法错误;新:rg stderr 洗短句)。
        {"grep_invalid_regex", ApprovedMigration::Kind::ErrorOnly, ""},
        // 16 KiB 单行墙:旧内核整行 20 万字符照吐,新内核截到 16384。
        {"grep_long_line_anchor", ApprovedMigration::Kind::CountOnlyCap16384, ""},
        // 非法 UTF-8:旧内核原字节进结果,新内核 base64 解码后过清洗。
        {"grep_illegal_utf8_content", ApprovedMigration::Kind::CountOnlySanitized, ""},
    };
    return table;
}

nlohmann::json DiffOne(const nlohmann::json& baseline, const nlohmann::json& current,
                       const ApprovedMigration* approved) {
    nlohmann::json report;
    report["id"] = baseline["id"];
    report["verdict"] = "equal";

    // 先裁决"批准的错误迁移":ECMAScript 认、Rust regex 不认的语法,基线
    // 多半是成功命中,新侧必须是稳定错误——若先比 is_error 会把这笔批准的
    // 迁移误判成"未批准出入",所以它排在最前面。
    if (approved != nullptr && approved->kind == ApprovedMigration::Kind::ErrorWithCode) {
        if (!current["is_error"].get<bool>()) {
            report["verdict"] = "unapproved";
            report["detail"] = "这套语法理应被 Rust regex 拒掉,实际却搜成功了";
            return report;
        }
        report["verdict"] = "approved";
        report["approved"] = std::string("语法迁移: 稳定错误 ") + approved->expect_code;
        return report;
    }

    if (baseline["is_error"] != current["is_error"]) {
        report["verdict"] = "unapproved";
        report["detail"] = "is_error 不一致";
        return report;
    }
    if (baseline["is_error"].get<bool>()) {
        // 两侧都报错:错误口径一致即对(报错文案不是合同)。
        if (approved != nullptr) {
            report["verdict"] = "approved";
            report["approved"] = "错误口径一致";
        }
        return report;
    }
    if (baseline["truncated"] != current["truncated"]) {
        report["verdict"] = "unapproved";
        report["detail"] = "truncated 不一致";
        return report;
    }
    const std::vector<std::string> base_hits = baseline["hits_sorted"];
    const std::vector<std::string> cur_hits = current["hits_sorted"];
    if (approved != nullptr) {
        switch (approved->kind) {
            case ApprovedMigration::Kind::HitsSubset:
            case ApprovedMigration::Kind::IncludeGlobSubset: {
                for (const std::string& hit : cur_hits) {
                    if (std::find(base_hits.begin(), base_hits.end(), hit) == base_hits.end()) {
                        report["verdict"] = "unapproved";
                        report["detail"] = "ignore 迁移出了旧命中集之外的新命中: " + hit;
                        return report;
                    }
                }
                if (cur_hits.empty()) {
                    report["verdict"] = "unapproved";
                    report["detail"] = "ignore 迁移把命中清零(连不该忽略的也没了)";
                    return report;
                }
                report["verdict"] = "approved";
                if (approved->kind == ApprovedMigration::Kind::IncludeGlobSubset) {
                    report["approved"] =
                        "显式 include glob 压过 ignore 规则(rg 优先级合同,与 Claude Code "
                        "Grep 同款):新命中仍是旧命中的子集(" +
                        std::to_string(base_hits.size()) + " -> " + std::to_string(cur_hits.size()) + ")";
                } else {
                    report["approved"] =
                        "ignore 语义迁移:新命中是旧命中的真子集(" +
                        std::to_string(base_hits.size()) + " -> " + std::to_string(cur_hits.size()) + ")";
                }
                return report;
            }
            case ApprovedMigration::Kind::CountOnlyCap16384: {
                if (cur_hits.size() != base_hits.size()) {
                    report["verdict"] = "unapproved";
                    report["detail"] = "命中数不一致";
                    return report;
                }
                for (const std::string& hit : cur_hits) {
                    const std::size_t colon2 = hit.find(':', hit.find(':') + 1);
                    const std::size_t line_len = hit.size() - colon2 - 1;
                    if (line_len > 16 * 1024) {
                        report["verdict"] = "unapproved";
                        report["detail"] = "长行没截到 16 KiB";
                        return report;
                    }
                }
                report["verdict"] = "approved";
                report["approved"] = "16 KiB 长行墙";
                return report;
            }
            case ApprovedMigration::Kind::CountOnlySanitized: {
                if (cur_hits.size() != base_hits.size()) {
                    report["verdict"] = "unapproved";
                    report["detail"] = "命中数不一致";
                    return report;
                }
                report["verdict"] = "approved";
                report["approved"] = "非法 UTF-8 正文清洗(锚点命中数不变)";
                return report;
            }
            case ApprovedMigration::Kind::ErrorOnly:
            case ApprovedMigration::Kind::ErrorWithCode:
                break;  // 到这里说明两侧都没报错:按全等继续
        }
    }
    if (base_hits != cur_hits) {
        report["verdict"] = "unapproved";
        report["detail"] = "hits_sorted 不一致";
        report["baseline_hits"] = base_hits;
        report["current_hits"] = cur_hits;
        return report;
    }
    return report;
}

int RunDiff(const fs::path& corpus_root, const fs::path& baseline_path, const fs::path& rg_exe,
            const fs::path& out_path) {
    MaterializeExcludedDirsFixture(corpus_root);

    std::ifstream baseline_file(baseline_path, std::ios::binary);
    if (!baseline_file.is_open()) {
        std::cerr << "基线打不开: " << PathToUtf8(baseline_path) << "\n";
        return 2;
    }
    const nlohmann::json baseline = nlohmann::json::parse(baseline_file);

    // 注入随包 rg 的生产 runner(smoke 真起 rg --version 精确校版本)。
    auto runner = std::make_shared<lubancode::tools::BundledRipgrepRunner>(rg_exe);

    const std::vector<Scenario> scenarios = BuildScenarios();
    std::size_t equal_count = 0;
    std::size_t approved_count = 0;
    std::size_t unapproved_count = 0;
    std::size_t missing_count = 0;
    nlohmann::json reports = nlohmann::json::array();
    nlohmann::json current_records = nlohmann::json::array();

    for (const Scenario& sc : scenarios) {
        const nlohmann::json current = RunScenarioWithTool(sc, corpus_root,
                                                           std::make_shared<SearchTool>(runner));
        current_records.push_back(current);

        const auto base_it = std::find_if(baseline.begin(), baseline.end(),
                                          [&sc](const nlohmann::json& rec) {
                                              return rec["id"].get<std::string>() == sc.id;
                                          });
        if (base_it == baseline.end()) {
            ++missing_count;
            nlohmann::json report;
            report["id"] = sc.id;
            report["verdict"] = "missing-in-baseline";
            reports.push_back(report);
            continue;
        }
        const ApprovedMigration* approved = nullptr;
        for (const ApprovedMigration& migration : ApprovedMigrations()) {
            if (std::string(migration.scenario) == sc.id) {
                approved = &migration;
                break;
            }
        }
        const nlohmann::json report = DiffOne(*base_it, current, approved);
        const std::string verdict = report["verdict"].get<std::string>();
        if (verdict == "equal") {
            ++equal_count;
        } else if (verdict == "approved") {
            ++approved_count;
        } else {
            ++unapproved_count;
        }
        reports.push_back(report);
        std::cout << sc.id << ": " << verdict;
        if (report.contains("approved")) {
            std::cout << "  (" << report["approved"].get<std::string>() << ")";
        }
        if (report.contains("detail")) {
            std::cout << "  [" << report["detail"].get<std::string>() << "]";
        }
        std::cout << "\n";
    }

    fs::create_directories(out_path.parent_path());
    std::ofstream f(out_path, std::ios::binary);
    nlohmann::json out;
    out["summary"] = {
        {"total", scenarios.size()},
        {"equal", equal_count},
        {"approved_migration", approved_count},
        {"unapproved", unapproved_count},
        {"missing_in_baseline", missing_count},
    };
    out["reports"] = reports;
    out["current"] = current_records;
    f << out.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) << "\n";
    f.close();

    std::cout << "差分汇总: 全等 " << equal_count << " / 批准迁移 " << approved_count
              << " / 未批准出入 " << unapproved_count << " / 基线缺失 " << missing_count << "\n";
    std::cout << "报告写入: " << PathToUtf8(out_path) << "\n";
    return unapproved_count == 0 && missing_count == 0 ? 0 : 1;
}

// ---- 基准 ---------------------------------------------------------------

struct BenchQuery {
    std::string id;
    std::string mode;
    std::string pattern;
    std::string glob;
    std::string note;
};

std::vector<BenchQuery> BuildBenchQueries() {
    return {
        {"literal_common_word", "grep", "SearchTool", "", "字面量,若干处命中"},
        {"regex_moderate", "grep", "std::[A-Za-z_]+<", "", "普通正则(模板类型)"},
        {"no_match", "grep", "definitely_absent_zzz_token_12345", "", "全仓无命中"},
        {"high_frequency", "grep", "the", "", "高频命中,触发 100 条截断"},
        {"glob_enum_cpp", "glob", "*.cpp", "", "glob 枚举 .cpp"},
    };
}

// 遍历语料目录一次,数文件数(不含跳过的 .git/build/node_modules/.evidence),
// 给基准报告一个"扫描面"参照,不依赖 SearchTool 内部计数。
std::size_t CountFiles(const fs::path& root) {
    std::size_t n = 0;
    std::error_code ec;
    static const std::vector<std::string> skip = {".git", "build", "node_modules", ".evidence"};
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;
    while (!ec && it != end) {
        std::error_code eec;
        if (it->is_directory(eec)) {
            const std::string name = PathToUtf8(it->path().filename());
            if (std::find(skip.begin(), skip.end(), name) != skip.end()) {
                it.disable_recursion_pending();
            }
        } else if (it->is_regular_file(eec)) {
            ++n;
        }
        it.increment(ec);
    }
    return n;
}

// 内存占用采样:试过 Windows psapi.h(K32GetProcessMemoryInfo),跟本仓
// 现有头文件顺序/工具链撞了车(struct PROCESS_MEMORY_COUNTERS 解析出
// "未知重写说明符"这类无关报错,像是宏或 include 顺序污染,一时排不清)。
// P0-1 是诊断批次,不为这一项"锦上添花"的指标去啃工具链坑,先如实留白;
// 交给 P0-7——那边本就是性能与稳定门的正式批次,要求"峰值内存"入基准表,
// 到时候另起炉灶排查(或换 GetProcessMemoryInfo+显式链 psapi.lib 的路子)。
std::int64_t ProcessPeakWorkingSetBytes() { return -1; }

double Percentile(std::vector<double> samples, double pct) {
    if (samples.empty()) return 0.0;
    std::sort(samples.begin(), samples.end());
    const double max_idx = static_cast<double>(samples.size() - 1);
    const std::size_t idx = static_cast<std::size_t>(std::min<double>(max_idx, pct / 100.0 * max_idx));
    return samples[idx];
}

int RunBench(const fs::path& corpus_dir_unused, const fs::path& out_path, const fs::path& target_dir) {
    (void)corpus_dir_unused;
    constexpr int kRounds = 7;

    const std::size_t file_count = CountFiles(target_dir);

    nlohmann::json out;
    out["target_dir"] = PathToUtf8(target_dir);
    out["file_count_scanned"] = file_count;
    out["rounds_per_query"] = kRounds;
    out["queries"] = nlohmann::json::array();

    for (const BenchQuery& q : BuildBenchQueries()) {
        SearchTool tool;
        std::vector<double> wall_ms;
        std::size_t last_hit_count = 0;
        bool last_is_error = false;
        for (int i = 0; i < kRounds; ++i) {
            nlohmann::json input;
            input["mode"] = q.mode;
            input["pattern"] = q.pattern;
            input["path"] = PathToUtf8(target_dir);
            if (!q.glob.empty()) input["glob"] = q.glob;

            const auto t0 = std::chrono::steady_clock::now();
            const Tool::Result result = tool.execute(input);
            const auto t1 = std::chrono::steady_clock::now();
            const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            wall_ms.push_back(ms);
            last_is_error = result.is_error;
            last_hit_count = std::count(result.content.begin(), result.content.end(), '\n');
        }

        // "冷/热盘分开"的穷人版:round 0 是这条查询在本进程里第一次真正
        // 触碰这批文件(不管 OS 页缓存此前是否被别的查询预热过),后续几轮
        // 明显更快时,说明读数主要是"文件系统遍历/IO"成本,不是正则本身;
        // 与 wall_ms_samples 一并落盘,不各自单独定论。
        std::vector<double> warm_rounds(wall_ms.begin() + 1, wall_ms.end());

        nlohmann::json rec;
        rec["id"] = q.id;
        rec["mode"] = q.mode;
        rec["pattern"] = q.pattern;
        rec["glob"] = q.glob;
        rec["note"] = q.note;
        rec["is_error"] = last_is_error;
        rec["output_lines_last_run"] = last_hit_count;
        rec["wall_ms_samples"] = wall_ms;
        rec["wall_ms_first_round"] = wall_ms.front();
        rec["wall_ms_p50"] = Percentile(wall_ms, 50);
        rec["wall_ms_p95"] = Percentile(wall_ms, 95);
        rec["wall_ms_min"] = *std::min_element(wall_ms.begin(), wall_ms.end());
        rec["wall_ms_max"] = *std::max_element(wall_ms.begin(), wall_ms.end());
        rec["wall_ms_p50_excluding_first_round"] = Percentile(warm_rounds, 50);
        rec["process_peak_working_set_bytes_after_query"] = ProcessPeakWorkingSetBytes();
        out["queries"].push_back(rec);

        std::cout << "bench " << q.id << ": p50=" << rec["wall_ms_p50"].get<double>()
                  << "ms p95=" << rec["wall_ms_p95"].get<double>() << "ms\n";
    }

    fs::create_directories(out_path.parent_path());
    std::ofstream f(out_path, std::ios::binary);
    f << out.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) << "\n";
    std::cout << "bench 写入: " << PathToUtf8(out_path) << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "用法:\n"
                  << "  search_golden_driver golden <输出JSON路径>\n"
                  << "  search_golden_driver diff <基线JSON> <rg可执行路径> [输出JSON]\n"
                  << "  search_golden_driver bench  <输出JSON路径> [语料目录]\n";
        return 2;
    }
    const std::string sub = argv[1];
    const fs::path out_path = Utf8ToPath(argv[2]);

#ifdef LUBANCODE_SEARCH_FIXTURES_DIR
    const fs::path corpus_root = fs::path(LUBANCODE_SEARCH_FIXTURES_DIR) / "corpus";
#else
    const fs::path corpus_root = fs::path("tests/fixtures/search/corpus");
#endif

    try {
        if (sub == "golden") {
            return RunGolden(corpus_root, out_path);
        }
        if (sub == "diff") {
            if (argc < 4) {
                std::cerr << "diff 需要 <基线JSON> <rg可执行路径> [输出JSON]\n";
                return 2;
            }
            const fs::path baseline_path = Utf8ToPath(argv[2]);
            const fs::path rg_exe = Utf8ToPath(argv[3]);
            const fs::path report_path =
                argc >= 5 ? Utf8ToPath(argv[4])
                          : baseline_path.parent_path() / "diff_report_vs_old_kernel.json";
            return RunDiff(corpus_root, baseline_path, rg_exe, report_path);
        }
        if (sub == "bench") {
#ifdef LUBANCODE_SOURCE_ROOT_DIR
            fs::path target_dir = fs::path(LUBANCODE_SOURCE_ROOT_DIR) / "src";
#else
            fs::path target_dir = fs::path("src");
#endif
            if (argc >= 4) {
                target_dir = Utf8ToPath(argv[3]);
            }
            return RunBench(corpus_root, out_path, target_dir);
        }
    } catch (const std::exception& e) {
        std::cerr << "异常: " << e.what() << "\n";
        return 4;
    }
    std::cerr << "未知子命令: " << sub << "\n";
    return 2;
}
