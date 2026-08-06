# 搜索服务

### search

`search` 必须同时有 `provider` 与非空字符串 `api_key`。`provider` 只认 `tavily`、`brave`、`serper`。真实密钥只填在本机配置，别进版本库。

```json
{
  "search": {
    "provider": "tavily",
    "api_key": "<在本机填入搜索服务密钥>"
  }
}
```

### 配搜索服务

编辑 `~/.lubancode/config.json`，写顶层 `search.provider` 与 `search.api_key`：

```json
{
  "search": {
    "provider": "brave",
    "api_key": "<只在本机填入搜索服务密钥>"
  }
}
```

把占位文字换成你本机保存的值。provider 只能用 `tavily`、`brave`、`serper`。别把真实值贴进源码、技能、测试、截图或 Git 提交。
