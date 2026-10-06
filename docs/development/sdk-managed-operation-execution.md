# Managed text operation execution: internal contract

This slice connects a fresh internal text session to SessionService's existing
queue, commit mutex, SessionExecution owner and V3 main writer. It does not open
public Managed SDK execution, authenticate a subject, run Policy callbacks,
resume an existing session, or upgrade a storage-only session.

## Admission and ownership

A distinct fresh-text opening consumes the actual ManagedSessionDirectory and
creation audit once, down Service → Runtime → Ledger → Manager. Its first V3
system metadata freezes `managed.text.v1`, RequestModel only, no tools or resume.
The opening tag is not stored in copyable Options. A text session remains behind
every generic Local entry gate. Its restricted execution assembly checks the
actual publication, first profile, session/run and empty tool registry, then uses
the same SessionExecution construction/publication tail as Local. Dispatch also
requires that installed owner; a profile string alone cannot admit execution.

The trusted internal host checks Policy outside the Service commit mutex, then
passes a complete immutable queue-front provenance and a nonzero dispatch
revision. Under that mutex the Service rechecks the exact front, original owner,
run, mode, live writer and installed execution. One confirmed schema4 dispatch
consumes one queue entry. An unknown append or publication gap retains its first
native fact, seals further admission/dispatch/final writes, and never retries an
ID. V3 binding follows the dispatch, before any turn/model work. Both producers
share the actual append half; Local still pops exactly once and retains its
original payload and failure stages.

## Versions and material

| Material | Version and transitions |
| --- | --- |
| Original accepted row / input artifact | schema3 / input2, original bytes unchanged |
| Before-dispatch rejection | schema3 Accepted → RejectedBeforeDispatch, unchanged |
| Text dispatch / final | schema4 Accepted → Dispatched → Final, exactly once |
| Main operation-turn anchor | existing event kind; explicit Managed layout v1 carries provenance hash |
| SDK result file | same five-field result payload as Local; exact original bytes, hash and length bound by final |

The storage-only reader continues to reject schema4. A separate owned execution
reader recognizes only this mixed version table. It checks full provenance,
unique dispatch/final, actual session/run/turn anchor and event ID/seq/hash,
result identity/hash/length/complete and exact input/result rosters. Captured
bytes hold no Writer, Service, resources or Policy. Capture stays within the
original commit lock and actual session lock, including Close's final capture.
Existing per-input/ledger/view caps stay in force; bounded V3/result captures add
explicit per-file and aggregate caps. These caps are not a total resident-memory
limit. No path mirror or database is introduced.

## Final and failures

Both paths construct the same SDK result payload. B actually consumes the shared
helper in a real internal text session. Managed publication uses immutable
Detailed native I/O, followed by a PowerLoss schema4 final. Artifact and append
receipts remain separate. Their native commitment survives any subsequent
in-memory allocation failure. No second Complete can overwrite the result,
reappend final, or read back an unknown into success. Missing turn/result facts
cannot produce a final. Close cancels only truly accepted pending entries; a
dispatched operation without a confirmed final keeps an incomplete close and
its original facts. Close drains execution before capturing/closing operation
and main writers and releasing the real SessionLock.

`completion_known` reports only this live owner's observation. It is not a
cross-process durable confirmation. Same-ID Managed recovery remains closed.

## Verification boundary

Native source must exercise a genuine reserved/published/finished fresh text
session, real execution resources, conditional queue consumption, actual V3
anchor and result/final files. Counterexamples cover unchanged storage refusal,
foreign/stale front, zero decision revision, missing execution, repeat final,
scope/run/anchor/result tampering, extra or missing originals, real native
unknown and committed-then-semantic-gap fences, and Close cleanup. Old Local
schema1/2, storage schema3, original CASE assertions and time limits stay intact.
No native tests, configure or compilation run locally. Remote CI will provide
the first native execution evidence after source review.
