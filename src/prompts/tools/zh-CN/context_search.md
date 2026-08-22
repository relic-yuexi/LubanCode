## description

在先前工具输出的落盘全文(artifact)里按关键词检索。工具结果太长时,请求里只留[artifact aNNNN ...] 引用(头尾预览);预览不够就用本工具搜全文,拿命中行号与块 id,再用 context_read 读出上下文。不可把预览的省略号当全文。

## param.artifact_id

[artifact aNNNN ...] 标记里的 aNNNN

## param.query

关键词(ASCII 大小写不敏感,中文按原文)

## param.max_results

最多回几条命中(默认 8)
