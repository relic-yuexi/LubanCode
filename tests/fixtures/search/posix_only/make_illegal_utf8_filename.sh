#!/usr/bin/env bash
# search 迁移单 P0-1:非法 UTF-8 文件名夹具——只在 POSIX 上现造,不进 git。
#
# 为什么不能像 corpus/ 其余夹具那样静态提交:
#   1. Windows(NTFS/Win32 API)的路径本质是 UTF-16,压根没有"文件名是一段
#      任意字节、可以不是合法 UTF-8"这回事——没法在 Windows checkout 出
#      这种文件名,git-for-windows 会在 checkout 时报错或按系统码页转写,
#      两边都不是"原样保留非法字节"。
#   2. 这份 P0-1 是在 Windows worktree 里做的(见 todo 交付说明),如实标注
#      "Windows 下不可测",不伪造。
#   3. 即便未来在 Linux/macOS CI 上生成,这类文件名也不该进 git 索引本身
#      (同样的编码风险会传染给别的平台的 checkout),所以走"运行时现造"
#      这条路,跟 corpus/excluded_dirs/(见 build_corpus.sh 里的说明)是
#      同一处理套路。
#
# 用法(仅 POSIX,如 WSL/Linux/macOS):
#   bash tests/fixtures/search/posix_only/make_illegal_utf8_filename.sh <目标目录>
# 在 <目标目录> 下现造几个文件名字节序列不是合法 UTF-8 的文件,内容里带一段
# 合法 ASCII 命中锚点(illegal_utf8_filename_needle),用于验证:
#   - grep 模式:遇到文件名不是合法 UTF-8 时不崩,要么跳过要么按字节透传,
#     具体走哪条由 P0-4/P0-5 跟真 rg 的 JSON bytes(base64)分支对齐后钉死。
#   - glob 模式:文件名过滤/NUL 分帧不因非法字节断帧或抛异常。
#
# 现掉的旧内核(P0-1 范围)本就是 std::filesystem::path 直接按 native 编码
# 走,理论上能收下任意字节的 POSIX 文件名;这份脚本产出的东西留给 P0-4
# 起真正引入 rg 子进程解析 stdout 路径字节时用,P0-1 本身不接 rg,不强求
# 现在就把这条接进 golden 驱动。

set -euo pipefail

if [ "$#" -ne 1 ]; then
  echo "用法: $0 <目标目录>" >&2
  exit 2
fi

TARGET="$1"
mkdir -p "$TARGET"

# 单字节延续码(0x80)独立出现:不是任何合法 UTF-8 序列的开头或延续。
printf 'illegal_utf8_filename_needle line one\n' > "$TARGET/$(printf 'lone_continuation_\x80.txt')"

# 0xFF 不是任何 UTF-8 字节合法值(UTF-8 编码规则里 0xFE/0xFF 永不出现)。
printf 'illegal_utf8_filename_needle line two\n' > "$TARGET/$(printf 'invalid_byte_\xff.txt')"

# 过长编码(overlong encoding):0xC0 0xAF 试图用两字节表示本该一字节表示的
# '/'字符,合法 UTF-8 解码器必须拒绝这种编码,即便字节序列本身"看着像"两
# 字节字符起手式。
printf 'illegal_utf8_filename_needle line three\n' > "$TARGET/$(printf 'overlong_\xc0\xaf_encoded.txt')"

echo "非法 UTF-8 文件名夹具已生成于: $TARGET"
ls -b "$TARGET"
