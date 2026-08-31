## description

在目录或单个文件里搜索,两种模式:mode="grep" 按正则(Rust regex 语法,与 ripgrep 同款)搜文件内容,命中的行按 文件:行号:行内容 返回;mode="glob" 按文件名通配(支持 * ? **)找文件,返回相对路径列表。默认从当前工作目录开始搜,遵守 .gitignore/.ignore/.rgignore,自动跳过 .git/、build/、node_modules/、.evidence/(运行时观察记录)和二进制文件;隐藏的项目文件(如 .github/、.clang-format)仍可搜。要搜被忽略的文件或观察记录,把 path 逐字点名到具体文件或目录即可。结果默认超过 100 条会截断并注明(可用 max_results 提前声明要多少);单条超长命中行截断到 16 KiB。搜索一律用本工具,不要在 run_command 里跑 rg 或 grep。

## param.mode

"grep" 搜文件内容(正则),"glob" 按文件名找文件(通配符)

## param.pattern

mode=grep 时是 Rust regex 正则表达式(ripgrep 同款:不支持 lookahead 与 backreference,支持 \p{Han} 等 Unicode 属性转义;要按字面搜正则元字符,配 fixed_strings=true);mode=glob 时是文件名通配符(支持 * ? **,ripgrep globset 语法)。不带 '/' 的写法(如 *.md)按文件名匹配,会递归找出整个目录树下所有同名文件,不管它在哪层子目录里;带 '/' 的写法(如 src/**/*.hpp、docs/**)按相对路径匹配,'**/' 表示零层或多层目录,写在开头就是'不管在不在根目录都算'。

## param.path

从哪里开始搜:给目录就递归遍历,给单个文件就只搜这一个。不填默认当前工作目录

## param.glob

仅 mode=grep 有效:按文件名或路径过滤要搜索的文件,不填就搜所有非二进制文件。语义跟 pattern 的 glob 写法一样:*.cpp 这种不带 '/' 的按文件名递归匹配任意目录下的文件;src/**/*.hpp 这种带 '/' 的按相对路径匹配。

## param.fixed_strings

仅 mode=grep 有效:true 时 pattern 按字面量逐字匹配,不解析正则元字符;默认 false,按 Rust regex 语法解析

## param.max_results

事前声明这次要多少条结果(软请求,只能调低不能调高):到数即停并注明已截断,缺省 100 条封顶。grep 按命中行计,glob 按文件计
