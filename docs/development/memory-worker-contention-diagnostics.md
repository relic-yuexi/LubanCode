# Memory worker failure diagnostics

This change adds test observations, not a worker fix. Windows full CI on B
`ca80881e` (run `37022611773`) failed two native cases in
`tests/unit/memory/test_project_memory.cpp`: concurrent queue workers returned an
error, and a real worker's same-ID update returned a failed completion. Windows
full CI on C `773a0ef2` (run `37022728510`) failed the concurrent-worker case.
Both runs preserve their original failure evidence. The test file, queue,
owner-lock and project-commit sources are byte-identical between these heads.
Neither original log printed the returned worker error. The cause remains
unproved; a lock-read race, exhausted queue-lock budget and other I/O errors are
candidates, not findings from the failed runs.

The two affected native cases keep every existing assertion, eight concurrent
jobs, same-ID update, call count, polling schedule and deadline. Production code,
`ProcessCrashDurability`, the 50-by-50-ms queue-lock retry budget, the project
lock budget, and immediate `BrokenLock` refusal do not change. No failed call is
retried or converted to success. The complete project-memory suite remains 46
native cases.

Each direct `RunPendingMemoryJobs` invocation records its start/end offsets,
elapsed time, and the original count or error. Observations follow the returned
call and do not count toward that call's elapsed time. Each worker uses its own
trace; the test reads those values after both futures finish. Queue observations
enumerate only this fixture's `memory-jobs/pending` and `failed` directories,
at most 32 entries each, and read only its `worker.lock/owner` (4 KiB cap).
Missing paths, capped enumeration and read errors remain distinct facts.

The same-ID case records the existing drain interval, existing supervisor wake
results, status and original completion error before the unchanged assertions.
It reads only that completion's lifecycle result (128 KiB cap), reporting raw
`commit_state`, `stages`, `status` and `outcome` fields as unverified observations.
It does not call receipt confirmation, acquire another project lock, reflush a
receipt or drain completions a second time. Existing APIs expose no worker
`Close` receipt; the diagnostic says so and never infers successful close from
an empty queue. A native worker's `RunPendingMemoryJobs` start/end cannot be
observed through this test-only change and is not fabricated.

Read failures print the returned read error. Diagnostics do not scan other
projects, discover temporary worker logs or read model/tool output. The two
original cases and all other assertions remain the remote acceptance gate.
No local native configure, compile, test or executable is used. A new private C
head runs fresh remote CI; its result is diagnostic evidence, not a claim that
the existing failures have been repaired.
