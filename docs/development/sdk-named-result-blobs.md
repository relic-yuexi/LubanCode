# Named result blobs: one Session capability

This slice replaces the bytes behind named V3 tool results. It does not replace
Journal, operation ledgers, frozen plans, project Memory or its separate CAS.
The default File path keeps existing tools, old sessions and material formats.
The public option is usable only after write, read, recovery and Close connect.

## Public contract

`named_results::v1` exposes owned STL values and one Provider/Store bundle.
`PublishNew`, bounded `Read` and bounded `SnapshotNames` share the actual locked
LocalTrusted `(workspace_key, session_id, binding_id)` scope. The provider does
not assign result IDs, authorize a Session, or receive a Writer or local path.
Null retains File. An explicit provider must preserve its frozen binding on
same-ID resume. Unsupported child/ancestor/new-ID domains fail before execution.

References retain the existing six-field artifact identity and logical
`artifacts/<name>` path. File previews retain real paths; external previews name
host-readable result references, never fictitious local files. Trusted SDK reads
still require ResultProjector and the existing Node/Session gates before export.

## Publication and ownership

The Session owns one revocable write lease and a separately retained read handle.
Bridge raw `capture-`, formal `res-`, Job `job-admission-`, Job completion and
listing writes all borrow this capability. Shared per-prefix numbering counts
orphan channels and temporary names and avoids foreground/Job/foreground clashes.

A whole-material lease covers numbering, all channels and metadata. It records
whether a provider was actually called. Partial publication, a thrown publishing
call or insufficient confirmation seals the first unknown before releasing the
serial gate. Reopening a facade, changing a request key, reading back or closing
cannot upgrade it. Known zero-publication rejection does not poison other work.
Names are create-new/no-replace, including collisions with identical bytes.
No rollback deletes published material. The first receipt stays owned and bounded.

File uses CreateImmutableFileDetailed and preserves its actual native receipt.
File/parent confirmation never proves an unconfirmed ancestor namespace or a
PowerLoss guarantee. The existing replacing AtomicWriteFile entry stays intact.
The stronger Session-wide unknown fence is intentional; legacy material formats,
successful previews and standalone File result-store behavior remain supported.

## Opening, readers and retirement

The real SessionLock supplies scope. A bounded canonical frozen plan and its SHA
in the initially adopted and effective system hostBindings identify storage.
Same-ID recovery verifies both before Provider Open or Writer continuation.
Generic Manager openings also reject an external binding without its provider;
CLI cannot silently reopen it as File. Old unbound sessions remain File-only.

Strict SDK result reads, Job parent confirmation/preview/recovery and automatic
ActionSummary all read through this capability. Source identities, metadata,
bytes, SHA, capture limits and summary context revision remain checked. Passive
Job Hold restores the original run without writes, dispatch or owner revival.

Close stops admission and cancels, retires bindings and joins host/coordinator,
then seals and waits for write transactions before releasing the live Session.
Queries retain only read ownership; Close does not wait for future readers to
disappear. The provider dies after the last read borrow, never retaining Writer,
SessionService or a strong public Session cycle. Provider callbacks/destruction
are cooperative; recursive provider reads and blocking lifecycle/Wait calls fail
before locks. Cancellation latches do not wait for provider serialization.

## Acceptance

Installed-header-only consumers store actual bytes in a second root, with no
named mirrors in the Session directory. Real Backend/tool/Job execution verifies
raw/formal identities, same-ID recovery, Close-after-read lifetime, automatic
budget-driven ActionSummary, and foreground/Job/foreground numbering. Actual
partial writes, after-write exceptions, bad receipts, namespace drift, cross-scope
references and reentrant callbacks must fail without retries or false success.
Default File retains all old result, immutable-publication and Job/deadline cases.
Validation runs on remote SDK-only/combined Linux, Windows, macOS and ASan CI.
Local work is source, pure data and static inspection only; no configure or native
execution. Source/consumer changes remain in one branch; CI wiring is integrated
separately before any claim of delivery.
