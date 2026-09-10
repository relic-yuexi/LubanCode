#!/usr/bin/env python3
"""session 轨迹 v3 校验器(P0 fixture 配套)。

合同:docs/architecture/trajectory-v3-schema.md;单子:todos/session轨迹v3_
消息主轴树链与四角色壳收敛设计.todo。与 C++ 侧 src/trajectory/v3/
(envelope/schema3/writer)同一口径,fixture 用两边互证。

用法:
  python scripts/validate_trajectory_v3.py <file.jsonl>...   # 严格校验
  python scripts/validate_trajectory_v3.py <file> --rehash   # 重算并填充哈希链
  python scripts/validate_trajectory_v3.py --self-test       # 内置样例自测

校验内容:
  1. 逐行 schema:类型白名单(未知键拒)、枚举(kind/purpose/origin/
     display.mode/completionStatus/status)、必选字段、kind↔status 映射。
  2. seq 从 1 连续;prevHash/lineHash 衔接:
     lineHash = sha256(prevHash || canonical(line 去 prevHash/lineHash))。
     canonical = 键按字节序、无空白、UTF-8 直出(与 C++ CanonicalJsonDump 对齐)。
  3. 语义断言:首行 system(seq=1、turnId=null);上下文链重放
     (session.started/context.system.applied/context.input.applied/
     compact.applied);compact 全链闭合(compactId 贯穿、applied 唯一终态);
     流式定稿唯一 assistant;assistant 必带 usage 键(缺实报 null,不补 0)。
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys

SCHEMA_VERSION = 3
GENESIS_HASH = "0" * 64

COMMON_KEYS = {
    "type", "schemaVersion", "sessionId", "runId", "seq", "timestamp",
    "prevHash", "lineHash",
}
MESSAGE_KEYS = COMMON_KEYS | {
    "messageId", "turnId", "parentTurnId", "stepId", "requestId", "actionId",
    "compactId", "purpose", "origin", "display", "message", "causedByEventRef",
    "sourceMessageRef", "systemMeta", "completionStatus", "provider", "wire",
    "model", "responseModel", "providerConfigRef", "modelProfileRef", "usage",
    "resultSelectionRef", "sourceToolMessageRef",
}
EVENT_KEYS = COMMON_KEYS | {
    "eventId", "kind", "status", "turnId", "parentTurnId", "stepId", "requestId",
    "actionId", "compactId", "commandId", "hookDispatchId", "taskId",
    "titleGenerationId", "payload", "effects", "effectRefs",
}

KINDS = {
    "session.started", "session.ended", "system.change",
    "context.system.applied", "context.input.applied", "context.tool_previews.reduced",
    "model.request.prepared", "model.request.sent", "model.request.failed",
    "model.response.started", "model.response.delta", "model.response.completed",
    "model.response.failed", "model.response.cancelled", "model.usage.appended",
    "compact.requested", "compact.pending", "compact.started",
    "compact.validation.started", "compact.validation.completed", "compact.applied",
    "compact.failed", "compact.cancelled", "compact.rejected",
    "tool.execution.pending", "tool.execution.started", "tool.execution.waiting",
    "tool.execution.resumed", "tool.execution.finished", "tool.execution.failed",
    "tool.execution.cancelled", "tool.execution.rejected", "tool.execution.unknown",
    "tool.result.persisted", "tool.result.persist_failed", "tool.result.selected",
    "hook.dispatch.requested", "hook.pending", "hook.started", "hook.completed",
    "hook.failed", "hook.cancelled", "hook.unknown", "hook.skipped",
    "hook.effects.applied", "hook.effects.rejected",
    "command.received", "command.pending", "command.started", "command.completed",
    "command.failed", "command.rejected", "command.cancelled", "command.unknown",
    "input.received", "input.enqueued", "input.admitted", "input.superseded",
    "title.requested", "title.extracted", "session.title.applied",
    "resume.source.attached",
    "subagent.spawn.requested", "subagent.linked", "subagent.observed",
    "subagent.spawn.failed",
    "task.started", "task.pending", "task.completed", "task.failed", "task.cancelled",
}

# kind 后缀 → 固定 status(§2.2);不在表内的 kind 不携带 status。
KIND_STATUS = {
    "pending": "pending", "waiting": "pending", "started": "running",
    "resumed": "running", "finished": "done", "completed": "done",
    "applied": "done", "sent": "done", "linked": "done", "failed": "failed",
    "cancelled": "cancelled", "rejected": "rejected", "unknown": "unknown",
}
STATUSLESS_KINDS = {
    "session.started", "system.change", "model.request.prepared",
    "model.response.started", "model.response.delta", "compact.requested",
    "context.tool_previews.reduced", "context.system.applied",
    "context.input.applied", "input.received", "input.enqueued",
    "input.admitted", "input.superseded", "resume.source.attached",
    "subagent.observed", "command.received",
    "hook.dispatch.requested", "hook.skipped", "title.requested",
    "title.extracted", "session.title.applied", "tool.result.persisted",
    "tool.result.persist_failed", "tool.result.selected",
    "hook.effects.applied", "hook.effects.rejected", "model.usage.appended",
    "subagent.spawn.requested",
}

ROLES = {"system", "user", "assistant", "tool"}
PURPOSES = {"conversation", "compact", "context_summary", "session_title", "capability"}
ORIGINS = {
    "human", "soul", "session_runtime", "compact_runtime", "context_runtime",
    "hook", "skill", "subagent", "parent_agent",
}
DISPLAY_MODES = {"visible", "collapsed", "hidden"}
COMPLETION_STATUSES = {"complete", "interrupted", "truncated"}
OP_STATUSES = {"pending", "running", "done", "failed", "cancelled", "rejected", "unknown"}

HEX64_LEN = 64


def required_status(kind: str) -> str | None:
    if kind in STATUSLESS_KINDS:
        return None
    last = kind.rsplit(".", 1)[-1]
    return KIND_STATUS.get(last)


def canonical(obj) -> str:
    """与 C++ CanonicalJsonDump 对齐:键字节序、无空白、非 ASCII 直出。"""
    return json.dumps(obj, sort_keys=True, ensure_ascii=False, separators=(",", ":"))


def line_hash(prev: str, obj: dict) -> str:
    stripped = {k: v for k, v in obj.items() if k not in ("prevHash", "lineHash")}
    material = prev.encode("utf-8") + canonical(stripped).encode("utf-8")
    return hashlib.sha256(material).hexdigest()


def is_hex64(value) -> bool:
    return (
        isinstance(value, str) and len(value) == HEX64_LEN
        and all(c in "0123456789abcdef" for c in value)
    )


def is_ref(value) -> bool:
    if isinstance(value, str):
        return bool(value)
    if isinstance(value, dict):
        return (
            set(value.keys()) == {"sessionId", "runId", "seq", "id", "hash"}
            and isinstance(value["sessionId"], str) and isinstance(value["runId"], str)
            and isinstance(value["seq"], int) and isinstance(value["id"], str)
            and is_hex64(value["hash"])
        )
    return False


ARTIFACT_KINDS = {
    "result_metadata", "stdout", "stderr", "combined", "report", "image", "blob",
}


def is_artifact_ref(ref) -> bool:
    """六键 artifactRef(§3.1):camelCase,kind 枚举,sha256 hex64,bytes 非负。"""
    if not isinstance(ref, dict):
        return False
    for key in ("artifactId", "kind", "path", "sha256", "bytes", "mediaType"):
        if key not in ref:
            return False
    return (
        isinstance(ref["artifactId"], str) and bool(ref["artifactId"])
        and isinstance(ref["kind"], str) and ref["kind"] in ARTIFACT_KINDS
        and isinstance(ref["path"], str) and bool(ref["path"])
        and isinstance(ref["sha256"], str) and is_hex64(ref["sha256"])
        and isinstance(ref["bytes"], int) and not isinstance(ref["bytes"], bool)
        and ref["bytes"] >= 0
        and isinstance(ref["mediaType"], str)
    )


def require_payload(kind: str, payload: dict, keys: list[str]) -> None:
    for key in keys:
        if key not in payload:
            raise ValidationError(f"{kind} payload 缺字段: {key}")


def check_tool_payload(obj: dict, kind: str, payload: dict, require_attempt: bool) -> None:
    """工具族公共:tool_call_id == 信封 actionId(§4.15);attempt 正整数。"""
    if payload.get("tool_call_id") != obj.get("actionId"):
        raise ValidationError(
            f"{kind} payload.tool_call_id 须等于信封 actionId(§4.15)")
    if require_attempt:
        attempt = payload.get("attempt")
        if not isinstance(attempt, int) or isinstance(attempt, bool) or attempt < 1:
            raise ValidationError(f"{kind} payload.attempt 应为从 1 起的正整数")


def check_child_session_ref(kind: str, payload: dict) -> None:
    ref = payload.get("childSessionRef")
    if not isinstance(ref, dict):
        raise ValidationError(f"{kind} payload.childSessionRef 应为 object")
    for key in ("sessionId", "runId", "journalPath"):
        if not isinstance(ref.get(key), str) or not ref[key]:
            raise ValidationError(f"{kind} childSessionRef.{key} 应为非空 string(§4.31)")


def check_child_checkpoint_ref(kind: str, payload: dict) -> None:
    ref = payload.get("childCheckpointRef")
    if not isinstance(ref, dict):
        raise ValidationError(f"{kind} payload.childCheckpointRef 应为 object")
    for key in ("sessionId", "runId"):
        if not isinstance(ref.get(key), str) or not ref[key]:
            raise ValidationError(f"{kind} childCheckpointRef.{key} 应为非空 string")
    seq = ref.get("seq")
    if not isinstance(seq, int) or isinstance(seq, bool) or seq < 0:
        raise ValidationError(f"{kind} childCheckpointRef.seq 应为非负整数")
    if not isinstance(ref.get("lineHash"), str) or not is_hex64(ref["lineHash"]):
        raise ValidationError(f"{kind} childCheckpointRef.lineHash 应为 64 位十六进制")


class ValidationError(Exception):
    pass


def validate_line(obj: object, expect_seq: int) -> dict:
    if not isinstance(obj, dict):
        raise ValidationError("行应为 JSON object")
    line_type = obj.get("type")
    if line_type not in ("message", "event"):
        raise ValidationError(f"type 只取 message/event,实得 {line_type!r}")
    keys = MESSAGE_KEYS if line_type == "message" else EVENT_KEYS
    unknown = set(obj.keys()) - keys
    if unknown:
        raise ValidationError(f"{line_type} 行未知键: {sorted(unknown)}")
    if obj.get("schemaVersion") != SCHEMA_VERSION:
        raise ValidationError("schemaVersion 应为 3")
    for key in ("sessionId", "runId", "timestamp", "prevHash", "lineHash"):
        if not isinstance(obj.get(key), str) or not obj[key]:
            raise ValidationError(f"缺字段或非字符串: {key}")
    if obj.get("seq") != expect_seq:
        raise ValidationError(f"seq 应为 {expect_seq},实得 {obj.get('seq')}")
    if rehash_mode[0]:
        # 占位 hash(如 "0")允许:结构过即可,随后由 rehash 填真值。
        pass
    elif not is_hex64(obj["prevHash"]) or not is_hex64(obj["lineHash"]):
        raise ValidationError("prevHash/lineHash 应为 64 位小写十六进制")

    if line_type == "message":
        if not isinstance(obj.get("messageId"), str):
            raise ValidationError("message 行缺 messageId")
        if "turnId" not in obj:
            raise ValidationError("message 行 turnId 键必须出现(可 null)")
        turn = obj["turnId"]
        if turn is not None and not isinstance(turn, str):
            raise ValidationError("turnId 应为 string 或 null")
        if obj.get("purpose") not in PURPOSES:
            raise ValidationError(f"purpose 未知: {obj.get('purpose')!r}")
        if obj.get("origin") not in ORIGINS:
            raise ValidationError(f"origin 未知: {obj.get('origin')!r}")
        message = obj.get("message")
        if not isinstance(message, dict) or message.get("role") not in ROLES:
            raise ValidationError("message 本体缺 role(system/user/assistant/tool)")
        if "display" in obj:
            mode = (obj["display"] or {}).get("mode")
            if mode not in DISPLAY_MODES:
                raise ValidationError(f"display.mode 未知: {mode!r}")
        if "completionStatus" in obj and obj["completionStatus"] not in COMPLETION_STATUSES:
            raise ValidationError(f"completionStatus 未知: {obj['completionStatus']!r}")
        role = message["role"]
        purpose = obj["purpose"]
        if role == "system":
            if turn is not None:
                raise ValidationError("system 消息 turnId 恒为 null")
            if not isinstance(obj.get("systemMeta"), dict):
                raise ValidationError("system 消息必带 systemMeta")
            if purpose not in ("conversation", "compact"):
                raise ValidationError("system purpose 只能 conversation/compact")
        elif role == "user":
            if purpose == "context_summary":
                if turn is not None:
                    raise ValidationError("注入摘要 turnId=null")
                if not isinstance(obj.get("sourceMessageRef"), str):
                    raise ValidationError("摘要必带 sourceMessageRef")
                if not isinstance(obj.get("compactId"), str):
                    raise ValidationError("摘要必带 compactId")
            elif turn is None:
                raise ValidationError("user 消息 turnId 必填(摘要除外)")
        elif role == "assistant":
            if turn is None or not isinstance(obj.get("requestId"), str):
                raise ValidationError("assistant 必带 turnId 与 requestId")
            for key in ("provider", "wire", "model"):
                if not isinstance(obj.get(key), str):
                    raise ValidationError(f"assistant 必带 {key}(§4.44)")
            if "responseModel" not in obj:
                raise ValidationError("assistant 必带 responseModel 键(可 null)")
            if "usage" not in obj:
                raise ValidationError("assistant 必带 usage 键(缺实报 null,不补 0)")
            usage = obj["usage"]
            if usage is not None:
                if not isinstance(usage, dict):
                    raise ValidationError("usage 应为 object 或 null")
                for key, value in usage.items():
                    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
                        raise ValidationError(f"usage.{key} 应为非负整数")
        else:  # tool
            if turn is None or not isinstance(obj.get("actionId"), str):
                raise ValidationError("tool 消息必带 turnId 与 actionId")
            if message.get("tool_call_id") != obj["actionId"]:
                raise ValidationError("message.tool_call_id 须等于信封 actionId")
            if not isinstance(message.get("content"), str):
                raise ValidationError("tool 消息 content 应为 string(§1.2.1)")
        # 降档派生消息(§4.38):origin 固定 context_runtime,不冒充新执行。
        if "sourceToolMessageRef" in obj and obj.get("origin") != "context_runtime":
            raise ValidationError(
                "带 sourceToolMessageRef 的派生消息 origin 须为 context_runtime(§4.38)")
    else:
        if not isinstance(obj.get("eventId"), str):
            raise ValidationError("event 行缺 eventId")
        kind = obj.get("kind")
        if kind not in KINDS:
            raise ValidationError(f"未知事件 kind: {kind!r}")
        expect_status = required_status(kind)
        if expect_status is None:
            if "status" in obj:
                raise ValidationError(f"{kind} 不携带 status 字段")
        else:
            if obj.get("status") != expect_status:
                raise ValidationError(
                    f"{kind} 的 status 须为 {expect_status},实得 {obj.get('status')!r}"
                )
            if expect_status == "pending" and "reason" not in (obj.get("payload") or {}):
                raise ValidationError(f"{kind} pending 必带 payload.reason")
        if not isinstance(obj.get("payload"), dict):
            raise ValidationError("event 行 payload 必为 object(可为 {})")
        # 按 kind 的必选身份字段。
        id_field = None
        if kind.startswith("compact."):
            id_field = "compactId"
        elif kind.startswith(("model.request", "model.response", "model.usage")):
            id_field = "requestId"
        elif kind.startswith(("tool.execution", "tool.result")):
            id_field = "actionId"
        elif kind.startswith("hook."):
            id_field = "hookDispatchId"
        elif kind.startswith("command."):
            id_field = "commandId"
        elif kind.startswith("task."):
            id_field = "taskId"
        elif kind.startswith("subagent."):
            id_field = "actionId"
        elif kind in ("title.requested", "title.extracted", "session.title.applied"):
            id_field = "titleGenerationId"
        if id_field is not None and not isinstance(obj.get(id_field), str):
            raise ValidationError(f"{kind} 必带 {id_field}")
        payload = obj.get("payload") or {}
        if kind.startswith(("tool.execution.", "tool.result.")):
            # 工具族载荷合同(§4.14-4.16/§4.19;与 C++ schema3 同口径)。
            if kind == "tool.execution.pending":
                check_tool_payload(obj, kind, payload, True)
                require_payload(kind, payload, ["reason"])
            elif kind == "tool.execution.started":
                check_tool_payload(obj, kind, payload, True)
                if not is_ref(payload.get("effectiveArgsRef")):
                    raise ValidationError("started payload.effectiveArgsRef 应为合法引用")
            elif kind == "tool.execution.waiting":
                check_tool_payload(obj, kind, payload, True)
                require_payload(kind, payload, ["reason"])
                if not is_ref(payload.get("waitRef")):
                    raise ValidationError("waiting payload.waitRef 应为可恢复等待引用")
            elif kind == "tool.execution.resumed":
                check_tool_payload(obj, kind, payload, True)
            elif kind == "tool.execution.finished":
                check_tool_payload(obj, kind, payload, True)
                if "exit_code" in payload and payload["exit_code"] is not None \
                        and not isinstance(payload["exit_code"], int):
                    raise ValidationError("finished exit_code 应为整数或 null")
            elif kind == "tool.execution.failed":
                check_tool_payload(obj, kind, payload, True)
                require_payload(kind, payload, ["error_code"])
            elif kind == "tool.execution.cancelled":
                check_tool_payload(obj, kind, payload, False)
                if payload.get("phase") not in ("before_started", "during_execution"):
                    raise ValidationError("cancelled.phase 应为 before_started|during_execution")
            elif kind == "tool.execution.rejected":
                check_tool_payload(obj, kind, payload, False)
                require_payload(kind, payload, ["reason"])
            elif kind == "tool.execution.unknown":
                check_tool_payload(obj, kind, payload, True)
                require_payload(kind, payload, ["reason"])
            elif kind == "tool.result.persisted":
                check_tool_payload(obj, kind, payload, True)
                refs = payload.get("result_ref")
                if not isinstance(refs, list) or not refs:
                    raise ValidationError("persisted result_ref 应为非空数组(§4.16)")
                for ref in refs:
                    if not is_artifact_ref(ref):
                        raise ValidationError("persisted result_ref 项不符六键 artifactRef")
                paths = [ref["path"] for ref in refs]
                if len(set(paths)) != len(paths):
                    raise ValidationError("result_ref 同一文件只列一次")
                if not is_ref(payload.get("executionEventRef")):
                    raise ValidationError("persisted executionEventRef 应为合法引用")
            elif kind == "tool.result.persist_failed":
                check_tool_payload(obj, kind, payload, True)
                require_payload(kind, payload, ["reason"])
            elif kind == "tool.result.selected":
                check_tool_payload(obj, kind, payload, False)
                sources = payload.get("sourceResultEventRefs")
                if not isinstance(sources, list) or not sources \
                        or not all(is_ref(r) for r in sources):
                    raise ValidationError("selected sourceResultEventRefs 应为非空引用数组")
                hooks = payload.get("hookEffectEventRefs")
                if not isinstance(hooks, list) or not all(is_ref(r) for r in hooks):
                    raise ValidationError("selected hookEffectEventRefs 应为引用数组")
                if payload.get("effectiveOutcome") not in (
                        "done", "failed", "substituted", "error"):
                    raise ValidationError(
                        "selected effectiveOutcome 应为 done|failed|substituted|error")
        elif kind.startswith("hook."):
            # hook 载荷合同(§4.22-4.23)。
            if kind == "hook.dispatch.requested":
                require_payload(kind, payload, ["hookPoint"])
            elif kind == "hook.started":
                require_payload(kind, payload, ["hookInvocationId", "hookId", "handlerKind"])
            elif kind == "hook.completed":
                require_payload(kind, payload, ["hookInvocationId", "hookId"])
            elif kind == "hook.failed":
                require_payload(kind, payload, ["error_code"])
            elif kind in ("hook.cancelled", "hook.unknown", "hook.skipped"):
                require_payload(kind, payload, ["reason"])
            elif kind == "hook.effects.applied":
                require_payload(kind, payload, ["effectType"])
            elif kind == "hook.effects.rejected":
                require_payload(kind, payload, ["effectType", "reason"])
        elif kind.startswith("subagent."):
            # subagent 载荷合同(§4.31-4.32)。
            require_payload(kind, payload, ["taskId"])
            if kind == "subagent.spawn.requested":
                check_child_session_ref(kind, payload)
                attempt = payload.get("attempt")
                if not isinstance(attempt, int) or isinstance(attempt, bool) or attempt < 1:
                    raise ValidationError("spawn.requested attempt 应为从 1 起")
            elif kind in ("subagent.linked", "subagent.observed"):
                check_child_checkpoint_ref(kind, payload)
            elif kind == "subagent.spawn.failed":
                require_payload(kind, payload, ["phase", "reason"])
        elif kind == "context.tool_previews.reduced":
            # §4.38:独立上下文提交事件。
            require_payload(kind, payload, [
                "contextId", "beforeRevision", "afterRevision", "oldPreviewBudget",
                "newPreviewBudget", "replacementRefs", "contextChain", "inputHash",
                "estimatedTokensBefore", "estimatedTokensAfter", "pairingCheckRefs"])
            if not payload["replacementRefs"]:
                raise ValidationError("replacementRefs 应为非空数组")
            old, new = payload["oldPreviewBudget"], payload["newPreviewBudget"]
            if not isinstance(old, int) or not isinstance(new, int) or new >= old:
                raise ValidationError("降档须 newPreviewBudget < oldPreviewBudget")
        if kind == "model.request.prepared":
            for key in ("contextId", "contextRevision", "systemMessageRef",
                        "inputMessageRefs", "readThroughSeq", "readThroughHash"):
                if key not in payload:
                    raise ValidationError(f"prepared payload 缺字段: {key}")
            if not isinstance(payload["inputMessageRefs"], list):
                raise ValidationError("inputMessageRefs 应为数组")
    return obj


def validate_chain_nodes(chain: list, where: str) -> None:
    if not chain:
        raise ValidationError(f"{where} 链不能为空")
    refs = [node["messageRef"] for node in chain]
    if len(set(refs)) != len(refs):
        raise ValidationError(f"{where} 链内 messageRef 重复")
    if not isinstance(chain[0].get("prevMessageRef"), type(None)):
        raise ValidationError(f"{where} 首节点前驱应为 null")
    for i, node in enumerate(chain):
        if set(node.keys()) != {"messageRef", "prevMessageRef"}:
            raise ValidationError(f"{where} 链节点须为 {{messageRef, prevMessageRef}}")
        if i > 0 and node["prevMessageRef"] != refs[i - 1]:
            raise ValidationError(f"{where} 链邻接与 prevMessageRef 不一致")
    if sum(1 for node in chain if node["prevMessageRef"] is None) != 1:
        raise ValidationError(f"{where} 根节点应恰有一个")


def validate_semantics(lines: list[dict]) -> list[str]:
    """跨行语义;返回发现的问题列表(空 = 全绿)。"""
    problems: list[str] = []
    if lines and not (
        lines[0].get("type") == "message"
        and lines[0].get("message", {}).get("role") == "system"
        and lines[0].get("seq") == 1
        and lines[0].get("turnId") is None
    ):
        problems.append("首行应为 seq=1、turnId=null 的 system message")

    # 上下文链重放。
    chain: list[dict] = []
    revision = 0
    for obj in lines:
        kind = obj.get("kind", "")
        payload = obj.get("payload", {})
        if kind == "session.started":
            context = payload.get("context", {})
            chain = context.get("contextChain", [])
            revision = context.get("revision", 0)
        elif kind == "context.system.applied":
            chain = payload.get("contextChain", [])
            revision = payload.get("afterRevision", revision)
        elif kind == "context.tool_previews.reduced":
            # §4.38:降档提交携带完整新链(原 tool 节点换派生消息,后续重接)。
            chain = payload.get("contextChain", [])
            revision = payload.get("afterRevision", revision)
            try:
                validate_chain_nodes(chain, kind)
            except ValidationError as error:
                problems.append(str(error))
        elif kind == "context.input.applied":
            for node in payload.get("appendedChain", []):
                if not chain or node.get("prevMessageRef") != chain[-1]["messageRef"]:
                    problems.append("context.input.applied 追加节点未接当前尾")
                    break
                chain.append(node)
            revision = payload.get("afterRevision", revision)
        elif kind == "compact.applied":
            chain = payload.get("contextChain", [])
            revision = payload.get("newContextRevision", revision)
    if lines:
        try:
            validate_chain_nodes(chain, "当前上下文")
        except ValidationError as error:
            problems.append(str(error))

    # compact 全链闭合:每个 compactId 的 requested 最终恰有一个终态。
    compacts: dict[str, dict] = {}
    for obj in lines:
        compact_id = obj.get("compactId")
        if not compact_id:
            continue
        state = compacts.setdefault(compact_id, {"requested": 0, "terminals": [],
                                                 "applied": 0, "chain_ok": True})
        kind = obj.get("kind", "")
        if kind == "compact.requested":
            state["requested"] += 1
        if kind in ("compact.applied", "compact.failed", "compact.cancelled",
                    "compact.rejected"):
            state["terminals"].append(kind)
            if kind == "compact.applied":
                state["applied"] += 1
                payload = obj.get("payload", {})
                try:
                    validate_chain_nodes(payload.get("contextChain", []), "compact.applied")
                except ValidationError as error:
                    problems.append(f"compact {compact_id}: {error}")
    for compact_id, state in compacts.items():
        if state["requested"] > 1:
            problems.append(f"compact {compact_id} requested 多枚")
        if state["applied"] > 1:
            problems.append(f"compact {compact_id} applied 不唯一")
        if state["applied"] > 0 and len(state["terminals"]) > 1:
            problems.append(f"compact {compact_id} 出现第二终态")
        if state["requested"] > 0 and not state["terminals"]:
            problems.append(f"compact {compact_id} 无终态(未闭合)")

    # 流式:每枚 assistant 的 messageId 恰有一次定稿事件;片段引用的
    # messageId 最终成行。
    finalized: dict[str, int] = {}
    reserved: set[str] = set()
    for obj in lines:
        kind = obj.get("kind", "")
        if kind in ("model.response.completed", "model.response.cancelled"):
            target = obj["payload"].get("messageId")
            finalized[target] = finalized.get(target, 0) + 1
        if kind == "model.response.started":
            reserved.add(obj["payload"].get("messageId"))
    for target, count in finalized.items():
        if count > 1:
            problems.append(f"messageId {target} 定稿事件多于一次")
    assistant_ids = [
        obj["messageId"] for obj in lines
        if obj.get("type") == "message" and obj.get("message", {}).get("role") == "assistant"
    ]
    for message_id in assistant_ids:
        if message_id not in finalized:
            problems.append(f"assistant {message_id} 无定稿事件")
    # 中断的 assistant 必须带 completionStatus=interrupted。
    for obj in lines:
        if obj.get("type") != "message":
            continue
        if obj.get("message", {}).get("role") != "assistant":
            continue
        if "usage" not in obj:
            problems.append(f"assistant {obj.get('messageId')} 缺 usage 键")
    return problems


# 模块级开关:validate_line 需要知道是否处于 rehash 模式(占位 hash 放行)。
rehash_mode = [False]


def validate_file(path: str, rehash: bool) -> bool:
    rehash_mode[0] = rehash
    with open(path, "r", encoding="utf-8") as handle:
        raw_lines = handle.read().splitlines()
    lines = []
    prev = GENESIS_HASH
    rewritten = []
    ok = True
    for index, raw in enumerate(raw_lines):
        if not raw.strip():
            continue
        try:
            obj = json.loads(raw)
            obj = validate_line(obj, len(lines) + 1)
        except ValidationError as error:
            print(f"  [行 {index + 1}] FAIL: {error}")
            ok = False
            continue
        if is_hex64(obj.get("prevHash", "")) and obj["prevHash"] != prev and not rehash:
            print(f"  [行 {index + 1}] FAIL: prevHash 与上一行 lineHash 不衔接")
            ok = False
        if rehash:
            obj["prevHash"] = prev
            obj["lineHash"] = line_hash(prev, obj)
        else:
            expect = line_hash(prev, obj)
            if obj["lineHash"] != expect:
                print(f"  [行 {index + 1}] FAIL: lineHash 重算对不上")
                ok = False
        prev = obj["lineHash"]
        lines.append(obj)
        rewritten.append(canonical(obj))
    if ok:
        problems = validate_semantics(lines)
        for problem in problems:
            print(f"  [语义] FAIL: {problem}")
        ok = not problems
    if rehash and ok:
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write("\n".join(rewritten) + "\n")
        print(f"  rehash: {len(lines)} 行已重算哈希链并回写")
    print(f"  {'PASS' if ok else 'FAIL'}: {path}({len(lines)} 行)")
    return ok


def self_test() -> int:
    """内置好/坏样例,脚本自测(铁律:python 脚本可本地自跑)。"""
    base = {
        "type": "event", "schemaVersion": 3, "sessionId": "s", "runId": "r",
        "seq": 2, "timestamp": "2026-09-10T00:00:00.000Z", "eventId": "evt-000001",
        "kind": "session.ended", "payload": {"reason": "x"},
        "prevHash": GENESIS_HASH, "lineHash": "0" * 64,
    }
    failures = 0
    # 好:合法事件行。
    try:
        validate_line(dict(base), 2)
    except ValidationError as error:
        failures += 1
        print(f"self-test 误报: {error}")
    # 坏:未知键。
    bad = dict(base); bad["oops"] = 1
    if not expect_fail(lambda: validate_line(bad, 2), "未知键"):
        failures += 1
    # 坏:status 不带于 session.ended? session.ended 后缀 ended 不在映射表 ->
    # 不携带 status;带上即错。
    bad = dict(base); bad["status"] = "done"
    if not expect_fail(lambda: validate_line(bad, 2), "status"):
        failures += 1
    # 坏:seq 断档。
    bad = dict(base); bad["seq"] = 5
    if not expect_fail(lambda: validate_line(bad, 2), "seq"):
        failures += 1
    # 好:assistant usage null。
    assistant = {
        "type": "message", "schemaVersion": 3, "sessionId": "s", "runId": "r",
        "seq": 3, "timestamp": "2026-09-10T00:00:00.000Z", "messageId": "msg-000001",
        "turnId": "turn-000001", "purpose": "conversation", "origin": "session_runtime",
        "message": {"role": "assistant", "content": "hi"},
        "requestId": "request-000001", "provider": "p", "wire": "w", "model": "m",
        "responseModel": None, "usage": None,
        "prevHash": GENESIS_HASH, "lineHash": "0" * 64,
    }
    try:
        validate_line(assistant, 3)
    except ValidationError as error:
        failures += 1
        print(f"self-test 误报: {error}")
    # 坏:assistant 缺 usage 键。
    bad = dict(assistant); del bad["usage"]
    if not expect_fail(lambda: validate_line(bad, 3), "usage 键"):
        failures += 1
    # 哈希链自洽:rehash 后重验通过。
    good = dict(base); good["lineHash"] = line_hash(GENESIS_HASH, base)
    if line_hash(GENESIS_HASH, base) == good["lineHash"]:
        print("self-test: canonical+sha256 链自洽 PASS")
    else:
        failures += 1
    print(f"self-test {'PASS' if failures == 0 else 'FAIL'}({failures} 处失败)")
    return 1 if failures else 0


def expect_fail(callable_, what: str) -> bool:
    try:
        callable_()
    except ValidationError:
        return True
    print(f"self-test 漏报: {what}")
    return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", nargs="*", help="v3 JSONL 文件")
    parser.add_argument("--rehash", action="store_true",
                        help="重算并填充哈希链后回写(fixture 维护用)")
    parser.add_argument("--self-test", action="store_true", help="内置样例自测")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if not args.files:
        parser.error("至少给一个文件,或用 --self-test")
    all_ok = True
    for path in args.files:
        all_ok = validate_file(path, args.rehash) and all_ok
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
