# http-recall:受控 HTTP 召回(PostUser)

用户消息落稳后,从个人知识库取一段材料追加为隐藏上下文。演示:

- `luban.http.request`:hook 自己的权限与预算(capabilities 申请 http,
  宿主授权才开;fixtures 里走 fake transport,零网络);
- `luban.context.append`:只交候选,采用经效果管道(PostUser 只许追加,
  不能回写原 user);
- `luban.state`:宿主持有的跨调用状态(去重);`luban.log.event`:
  结构化日志,失败留痕不拦会话(keep_original)。

生产接入:装到 `~/.lubancode/hooks/http-recall/` 后,须在宿主配置里给这个
包授权 HTTP 与目标域名——发现不等于授权。跑法:`lubancode hook test .`。
