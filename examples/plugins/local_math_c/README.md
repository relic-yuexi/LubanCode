# local-math-c — process 插件示例(C 可执行文件)

C 源码不能直接跑;正路是作者预编成独立 executable,manifest 的 `command`
指产物,用户机器不需要编译器。与 native 插件(共享库)的差别:executable
是独立进程,没有跨 CRT free、没有宿主 ABI,崩了只坏当次调用——对"一枚
本地工具"往往比共享库省心(单子「先答五个问题」四)。

## 构建

```bash
gcc -O2 -o local_math_c main.c        # Linux / macOS(产物无扩展名)
gcc -O2 -o local_math_c.exe main.c    # Windows(MinGW;MSVC 用 cl /O2 main.c)
```

Windows 下 plugin.json 的 `command` 记得带 `.exe`。

## 安装

`plugin.json` 与编译产物放同一目录,整个拷进
`~/.lubancode/plugins/local-math-c/`。`${plugin_dir}` 占位符让目录搬哪儿
都能跑。重启 LubanCode,`/plugins` 应见 `local-math-c: 1 个工具`。

## 自测

```bash
echo '{"protocol":1,"call_id":"t1","plugin":"local-math-c","tool":"add","arguments":{"a":1.5,"b":2},"context":{}}' | ./local_math_c
```

期望 `{"protocol":1,"call_id":"t1","ok":true,...,"text":"3.5"}`。

## 要点

- 示例用 `sscanf` 抠字段只够演示;正经插件用 jsmn 之类小解析库。
- 超时默认 10s(这枚工具毫秒级,给窄墙);到点杀整棵进程树。
- stdout 恰好一份 JSON 是铁律;调试信息写 stderr。
