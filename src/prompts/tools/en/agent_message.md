## description

Hand incremental requirements to a running subagent. It only interjects: it does not create tasks (that is the agent tool's job), does not interrupt the tool the subagent is currently executing, and does not revive finished tasks. When you must use it: when the user supplements, modifies, or withdraws a requirement in the main conversation and it affects a running subagent, call this tool first to forward the increment to it, then continue your reply; when the user names a specific task, deliver precisely by task_id; when one requirement affects several subagents, send one message to each in turn—there is no broadcast; if the target is unclear, ask the user first instead of guessing from a similar title; forward only the increment, never repeat the whole task description; do not skip the forwarding just because you also remember it yourself—the subagent has its own context and cannot see new messages in the main session; only after this tool returns queued may you tell the user it has been delivered; before the call, never present the forwarding as a done deed. How to write message: first quote the user's original words verbatim (starting with "User's original words:"); put your own explanation in a separate column (starting with "[Main agent's additional context]") and never pass inference off as the user's requirement. The message is delivered before the subagent's next model request, after its current tool finishes; it is treated as an ordinary user-side supplement—not a permission confirmation; slash commands inside it will not be executed, and it cannot be used to bypass any confirmation. The roster of running tasks is the "running subagents roster" attached to each user message.

## param.task_id

Stable task number of the running subagent (see #N in the "running subagents roster").

## param.message

The incremental requirement sent to that task; state what changed, why, and how acceptance is affected. First quote the user's original words verbatim (starting with "User's original words:"), then put the main agent's own explanation in a separate column (starting with "[Main agent's additional context]").
