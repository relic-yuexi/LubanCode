# local-math — process 插件示例(Python)

最短的 process 插件:一枚 `add` 工具,Python 写成。结构与协议与
`lubancode plugin init python` 生成的脚手架一致,这份是手写版,带注释。

## 安装

```bash
# Windows
mkdir "%USERPROFILE%\.lubancode\plugins\local-math"
copy plugin.json runner.py "%USERPROFILE%\.lubancode\plugins\local-math\"

# Linux / macOS
mkdir -p ~/.lubancode/plugins/local-math
cp plugin.json runner.py ~/.lubancode/plugins/local-math/
```

Windows 上 `plugin.json` 里的 `command` 写 `python3` 找不到的话改成
`python` 或 `py`(或绝对路径/venv 解释器)。重启 LubanCode,`/plugins`
应见 `local-math: 1 个工具`;模型侧工具名是 `plugin__local-math__add`。

## 自测

```bash
echo '{"protocol":1,"call_id":"t1","plugin":"local-math","tool":"add","arguments":{"a":1,"b":2},"context":{}}' | python3 runner.py
```

期望输出一份 JSON,`ok` 为真,`content[0].text` 是 `3`。

## 要点

- stdout 是结果专线;调试日志写 stderr。
- `command`/`args` 来自 manifest,不经 shell——参数里的引号、空格、
  `&;|` 不可能变成命令。
- 子进程环境是最小集(PATH + 系统变量 + allowlist 点名),宿主环境里的
  密钥一概不递。
- 超时默认 30s,到点杀整棵进程树;ESC 同一条取消路。
