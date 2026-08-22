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
