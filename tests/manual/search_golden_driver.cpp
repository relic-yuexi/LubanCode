// search 内置 ripgrep 后端迁移单(P0-1)的旧内核照相机。
//
// 不进 ctest,手动跑:
//   search_golden_driver golden  <输出 JSON 路径>
//       把 tests/fixtures/search/corpus/ 这批夹具逐条场景喂给现有
//       SearchTool::execute(),记录旧内核(std::regex + recursive_directory_
//       iterator)的输出,按"路径集合/命中行集合"归一化后落盘,供 P0-5
//       批次拿新 ripgrep 后端跑同一份场景表做自动化差分。
//   search_golden_driver bench   <输出 JSON 路径> [语料目录=src]
//       对旧内核跑一组基准查询(纯字面量/正则/无命中/高频命中/glob),
//       每档跑若干轮取 P50/P95,连同遍历文件数、耗时一并落盘。默认语料
//       是本仓 src/ 目录(medium 档);可传别的目录路径覆盖。
//
// 两个子命令共享同一份"调用 SearchTool、量时钟"的骨架,分开落盘方便
// P0-1 报告分别引用。
//
// golden JSON 格式(数组,每个场景一个对象):
//   {
//     "id": "grep_chinese",                 场景名,P0-5 差分表按它对齐
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
// iterator 顺序本就不是合同的一部分,以后 ripgrep 的并行 walker 顺序更不
// 会与之相同。

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "tools/path_utils.hpp"
#include "tools/search.hpp"


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

nlohmann::json RunScenario(const Scenario& sc, const fs::path& corpus_root) {
    SearchTool tool;
    nlohmann::json input;
    input["mode"] = sc.mode;
    input["pattern"] = sc.pattern;
    const fs::path abs_path = corpus_root / Utf8ToPath(sc.rel_path);
    input["path"] = PathToUtf8(abs_path);
    if (!sc.glob.empty()) {
        input["glob"] = sc.glob;
    }

    const Tool::Result result = tool.execute(input);

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
