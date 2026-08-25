## description

按稳定 id 读先前工具输出落盘全文(artifact)的一段:给 chunk_id(context_search 命中给的)或 line_start(1 起)+line_count。单次最多 32 KiB,超了会拒绝并给可用范围。全文真本按 sha256 校验,hash 不合的内容不会被供给。

## param.artifact_id

[artifact aNNNN ...] 标记里的 aNNNN

## param.chunk_id

块 id(如 c0003);给了就按块读

## param.line_start

起始行(1 起;与 chunk_id 二选一)

## param.line_count

读几行;0 = 读到结尾

## summarize_guidance

只有原文很长、逐段读取代价更高时,才可设 summarize=true 请 cheap 模型按需摘要;这会额外消耗模型 token。摘要作为本次工具结果追加,不改旧消息。

## param.summarize

按需调用 cheap 模型摘要整枚 artifact;会额外消耗 token,不可与 chunk_id/行窗同用
