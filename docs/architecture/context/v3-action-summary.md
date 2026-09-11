# 新 action 整批预算与摘要

本页对应 B2 实现。验证走 GitHub macOS CI；没有本地构建结果，也不据假后端断言真实摘要质量。

## 首次定形

工具组闭合后，loop 只给这批尚未发送结果分配预算。固定输入由 adapter 最终 JSON 取 system/messages/tools、instructions/input 或 contents 等输入字段。输出参数和传输参数不计入输入。总 UTF-8 字节只取整一次：`ceil(bytes/4)`。输出预留、512 token 协议余量另列。JSON 转义最坏扩张按每字节六字节预留，属于序列化空间，不是 token 校正系数。

分配按调用均摊余量，小结果先保全。每项仍有原 tool_call_id；旧消息不参与重新选取。原始结果先入 ResultStore，落 `tool.result.persisted`，再产生预览或摘要。整批最低材料仍装不下，保住已执行结果后停止；不重跑工具、不发送已知超限请求。

四个生产 adapter 提供实际序列化入口。没有该入口的旧测试/trace backend 仍走原预检，不能把它算作 adapter 预算验收。图片、音频、视频、文件二进制与不透明思考没有统一预算策略时，明确报未计量，不能拿文本 bytes/4 补零放行。

## 模型路由与界限

整批分配确实缩小某项预算时，可调用 action 摘要。默认用当前 backend 和明确的 model，独立构造无工具的请求；不会继承主会话历史。主会话、子代理与 workflow 节点均接到同一服务。

摘要窗口为 `min(主模型窗口,32768)`，输出上限 1024 token，协议余量 512 token。每次实际请求均按摘要 adapter 的最终输入验容量，并读取有效输出上限。整批最多八次调用，最多两层：按 combined 原文分块，再至多汇总一次。源字节边界按 UTF-8 对齐。块数、窗口、层数不够，明确收口；无收益不继续递归。

材料取已经落稳的 combined 原文，核对 persisted 事件、action 身份、artifact 引用和 SHA-256。raw_payload 等额外材料只列证据路径，不声称它们全文进入摘要请求。原始捕获不完整时，摘要仍保留 incomplete 与原因。

## 持久合同

新增 message purpose `action_summary`。内部 system、user 和 assistant 默认隐藏，独立 turnId/stepId/requestId；不接纳进 main 链。`model.request.prepared` 保存 provider/wire/model、来源 action、persisted 引用、块偏移/长度、层次、估算和输出预留。assistant 独占这次 usage；未报告则为 null，不串入主模型 usage。

候选须为 JSON：非空 `summary`，以及字符串数组 `side_effects`、`open_items`、`evidence`。工具请求、截断、格式错、无收益、超过最终字节帽均拒收。宿主另填执行终态、`execution_already_occurred`、捕获状态、原始事件引用和全部证据路径，模型不能覆盖这些事实。

`tool.result.summary.finished` 是候选终态，不是采用。它保存 `state`（accepted/rejected/failed/cancelled）、来源 revision、persisted 引用、candidateMessageRefs、modelCalls、预算与产出字节、previewSha256、路由和失败原因。

只有 accepted 候选可用于 `tool.result.selected.summaryEventRef`。随后写 tool 消息并成功接纳，运行时才发布同一正文。恢复读取核对 action、来源、候选用途、先后顺序和预览 hash；不重跑摘要，也不重新截取。候选期间上下文 revision 变化则拒收。持久化失败挡住发布；已独立提交的结果仍保留。

摘要能力失败可回到确定性范围预览；最终请求仍须过整体门禁。它不是修改旧 step 的入口，历史 step 摘要由 compact 处理。
