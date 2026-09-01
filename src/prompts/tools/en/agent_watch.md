# agent_watch

## description

Inspect the supervision snapshot of sub-agent tasks (state, stage, health, silence age, retry count), optionally sleeping until a task changes. It does not detect failures — the in-host AgentSupervisor does that continuously; this tool only folds the ledger into a bounded read-only snapshot.

Read-only rules: it never stops a task, never sends messages, never causes any side effect. To add instructions to a running sub-agent use agent_message; to stop one use the Agent Dock x; neither belongs here.

Usage and waiting discipline: query once without wait_ms and take the returned revision; to wait later, pass it back as after_revision together with wait_ms (0..30000 ms). Revision advances, a task reaching a terminal state, user steering, parent cancellation, or session close all wake the wait early. Wait only for state changes — do not poll on a short cycle: never loop with wait_ms=500-style short waits. Waiting costs the host nothing; sleep until something changes.

Empty task_ids: main sees all running root tasks, a sub-agent sees only its direct children; explicit ids are capped at 16. include=summary (default) returns the short snapshot; include=events additionally returns up to 50 structured events per task after after_revision (kinds and tool names only — no text or thinking); include=diagnostic is main-only and returns diagnostic counters and stable error codes, likewise without text, thinking, secrets, or full tool arguments.

Reading the result: health=healthy is normal; recovering means automatic reconnection after a stream break (with the attempt count); suspect_* means the host has flagged the stage soft line — hard timeouts and the total wall clock still apply, no panic intervention needed. When a task is terminal (done/failed/cancelled), the state is in tasks[].state and the full result arrives as its tool result; do not keep probing with this tool.

## param.task_ids

Task numbers to watch (the #N from the roster). Empty = main sees all running root tasks, a sub-agent sees its direct children. At most 16; a sub-agent naming a non-direct child is rejected.

## param.after_revision

The revision returned by your previous call. If the snapshot would be identical and wait_ms>0, the tool sleeps until a task changes or the wait times out; otherwise it returns the fresh snapshot immediately.

## param.wait_ms

How long to wait at most, in milliseconds, capped at 30000; 0 = snapshot only, no waiting. The wait is bounded and free: user steering, task cancellation, or session close wake it early. Do not poll on a short wait_ms cycle — wait for state changes.

## param.include

Output level: summary (default, short snapshot) / events (adds the structured event stream, kinds and tool names only) / diagnostic (main-only: diagnostic counters and stable error codes).
