#!/usr/bin/env bash
# 生成 tests/fixtures/search/corpus/ 全量夹具。
# 幂等:先清空再重建,方便复跑核对。
# 用法:cd 到仓库根后 `bash tests/fixtures/search/build_corpus.sh`
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CORPUS="$ROOT/corpus"

rm -rf "$CORPUS"
mkdir -p "$CORPUS"

# 1. 中文内容
mkdir -p "$CORPUS/chinese"
printf '第一行\n这里有关键词chinese_needle\n第三行\n' > "$CORPUS/chinese/chinese.txt"

# 2. CRLF 行结尾
mkdir -p "$CORPUS/crlf"
printf 'line one\r\nneedle_crlf here\r\nline three\r\n' > "$CORPUS/crlf/crlf.txt"

# 3. 空文件
mkdir -p "$CORPUS/empty"
: > "$CORPUS/empty/empty.txt"

# 4. 二进制文件(含 NUL 字节,且字节流里藏一个"看似命中"的字面量,
#    验证二进制探测优先于正则匹配生效——不该被当成命中)
mkdir -p "$CORPUS/binary"
printf 'BIN\x00ARY_needle_bin\nb\r\n' > "$CORPUS/binary/blob.bin"

# 5. 超长行(约 200000 字符,含一个命中锚点在中间)
mkdir -p "$CORPUS/long_line"
{
  head_pad=$(printf 'x%.0s' $(seq 1 100000))
  tail_pad=$(printf 'y%.0s' $(seq 1 100000))
  printf 'short_line_before\n'
  printf '%sneedle_long_line_anchor%s\n' "$head_pad" "$tail_pad"
  printf 'short_line_after\n'
} > "$CORPUS/long_line/long_line.txt"

# 6. 100+ 命中(150 行同一字面量,验证 100 条截断)
mkdir -p "$CORPUS/many_hits"
for _ in $(seq 1 150); do
  printf 'hit_line_150\n'
done > "$CORPUS/many_hits/many_hits.txt"

# 7. 隐藏文件/隐藏目录
mkdir -p "$CORPUS/hidden/.hidden_dir"
printf 'hidden_needle in dotfile\n' > "$CORPUS/hidden/.hidden_file.txt"
printf 'hidden_dir_needle nested under dot-dir\n' > "$CORPUS/hidden/.hidden_dir/inside.txt"
printf 'visible_needle not hidden\n' > "$CORPUS/hidden/visible.txt"

# 8. ignore 文件(.gitignore/.ignore/.rgignore)三家各拦一个,kept.txt 三家都不管
mkdir -p "$CORPUS/ignore"
printf 'ignored_by_gitignore.txt\n' > "$CORPUS/ignore/.gitignore"
printf 'ignored_by_dot_ignore.txt\n' > "$CORPUS/ignore/.ignore"
printf 'ignored_by_rgignore.txt\n' > "$CORPUS/ignore/.rgignore"
printf 'ignore_needle via gitignore\n' > "$CORPUS/ignore/ignored_by_gitignore.txt"
printf 'ignore_needle via dot ignore\n' > "$CORPUS/ignore/ignored_by_dot_ignore.txt"
printf 'ignore_needle via rgignore\n' > "$CORPUS/ignore/ignored_by_rgignore.txt"
printf 'ignore_needle kept, no ignore rule matches\n' > "$CORPUS/ignore/kept.txt"

# 9. 宿主硬排除目录:.git/build/node_modules/.evidence
# 注意:这一组不进 git 提交——目录里必须有一个字面量叫 ".git" 的子目录才
# 能验真 ShouldSkipDir("...","\".git\"") 这条硬排除,但 git 天生不认"普通
# 内容里有个叫 .git 的子目录"这种东西(仓库边界保留字),没法把它当常规文件
# 提交。build/ 这层还会被仓库根 .gitignore 的 "build/" 规则连坐忽略。
# 处理办法:这段仅供本地手动生成/核对用;golden 驱动与未来 P0-5 差分工具
# 各自在运行时把这组目录现造到临时目录里(见 search_golden_driver.cpp 里
# 的 MaterializeExcludedDirsFixture),不依赖这里落盘的静态文件。
# 仓库 .gitignore 已加一条 tests/fixtures/search/corpus/excluded_dirs/,
# 所以这里生成出来也不会被 git 追踪,不用操心。
mkdir -p "$CORPUS/excluded_dirs/dotgit_dir/objects" \
         "$CORPUS/excluded_dirs/build" \
         "$CORPUS/excluded_dirs/node_modules/pkg" \
         "$CORPUS/excluded_dirs/.evidence"
printf 'excluded_needle in fake git objects\n' > "$CORPUS/excluded_dirs/dotgit_dir/objects/pack_marker.txt"
printf 'excluded_needle in build output\n' > "$CORPUS/excluded_dirs/build/generated.txt"
printf 'excluded_needle in node_modules\n' > "$CORPUS/excluded_dirs/node_modules/pkg/index.js"
printf 'excluded_needle in evidence log\n' > "$CORPUS/excluded_dirs/.evidence/subagent-1.log"
printf 'excluded_needle in real tracked file\n' > "$CORPUS/excluded_dirs/real.txt"
# dotgit_dir 建好后改名成真正的 ".git"——分两步是为了不让途中任何工具
# 把中间态误判成嵌入式 git 仓库(见 build_corpus 使用说明)。
mv "$CORPUS/excluded_dirs/dotgit_dir" "$CORPUS/excluded_dirs/.git"

# 10. 嵌套 glob 语料
mkdir -p "$CORPUS/nested_glob/src/sub/deeper" "$CORPUS/nested_glob/docs/sub"
printf 'a.cpp\n' > "$CORPUS/nested_glob/src/a.cpp"
printf 'b.cpp\n' > "$CORPUS/nested_glob/src/sub/b.cpp"
printf 'c.cpp\n' > "$CORPUS/nested_glob/src/sub/deeper/c.cpp"
printf '# readme\n' > "$CORPUS/nested_glob/docs/readme.md"
printf 'notes\n' > "$CORPUS/nested_glob/docs/sub/notes.md"
printf '# top\n' > "$CORPUS/nested_glob/top.md"

# 11b. ECMAScript 独有语法(backreference、lookahead)的命中锚点——旧内核
#      (std::regex ECMAScript)认得,Rust regex 默认引擎设计上不支持,留给
#      迁移表当证据用,不是"必须保留"的合同用例。
mkdir -p "$CORPUS/regex_only_ecmascript"
printf 'plain line, no special syntax needed here\ntoken_token_repeat via backreference\nprefix lookahead_marker_present suffix\n' \
  > "$CORPUS/regex_only_ecmascript/regex_only_ecmascript.txt"

# 12. 非法 UTF-8 正文(文件名合法,内容含非法字节序列 + 一段合法 ASCII 命中锚点)
mkdir -p "$CORPUS/illegal_utf8_content"
printf 'valid line before\nutf8_needle_anchor \x80\xff\xc0\xaf trailing\nvalid line after\n' \
  > "$CORPUS/illegal_utf8_content/bad_utf8.txt"

echo "corpus 生成完毕: $CORPUS"
