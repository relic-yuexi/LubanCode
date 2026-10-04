# Project memory commit gate

This batch extracts one internal, typed **project/upsert** commit gate. The CLI
worker calls it for project `upsert` jobs. User memory, forget, verify, rebuild,
queue ownership and worker launch keep their current paths. There is no public
SDK write API, candidate substitution or CLI helper launch in this gate.

## Inputs and ownership

The caller supplies value-owned project/memory/lifecycle paths, workspace key,
operation key, source session/event references and `SaveRequest`. It remains
responsible for selecting an authorized project. The gate validates project
scope, safe paths and UTF-8, owns the existing project `OwnerLock` during the
whole call, and reads no global CLI configuration. Cancellation may stop before
the intent or between stages; it never deletes a visible write to pretend that
nothing happened. A future SDK session must join its active call before releasing
the session writer. This batch borrows no session writer.

## Commit and receipt

The fixed request identity includes every save field, source reference and target
identity. Its canonical JSON SHA-256 is saved in the intent and result. Under the
project lock, the same operation key and SHA may return a validated original
receipt; a different SHA conflicts. Malformed files, broken links and unsupported
legacy records are explicit errors. An intent with no result is indeterminate
and **must not replay** the mutation automatically.

The checksums detect corruption and bind the requested bytes; they are not an
authentication mechanism against an actor who can rewrite every local file.
Committed duplicate reads verify the immutable snapshot, the request binding and
the ordered mutation stages. They then reflush the unchanged result bytes to
confirm result durability. A previous result-directory flush failure can thus be
confirmed without repeating any topic mutation. CLI completion readers use the
same existing-only confirmation path for new receipts. It cannot create an intent
or start an upsert. Legacy schema-1 project receipts have no request binding and
are explicitly refused for same-key replay; no legacy record is rewritten.

Preparation reuses topic-store upsert assembly: existing creation time, source
sessions and occurred-at inheritance, canonical schema-3 filename, evidence
fingerprints and index/catalog content retain their meanings. The write stages
are intent, immutable topic snapshot, topic replacement, old-name cleanup,
catalog, index and result. Their platform `WriteOutcome` values remain visible in
the typed receipt. Result success requires all requested stages to confirm
`ProcessCrashDurability`; the result also keeps the established CLI outcome keys
so existing completion readers can read successful new receipts.

`NotStarted` means no topic mutation began. `Committed` means the topic and its
receipt confirmed the requested durability. `Indeterminate` means a visible
mutation or an intent without a trustworthy final result needs explicit recovery.
Failed receipt writes do not authorize replay; old topics are never restored by
delete-and-rewrite. These are per-file guarantees, not a multi-file transaction,
an OS sandbox, or a promise that newly created ancestor directories survive a
power loss. Cancellation and process death cannot make an uncertain commit safe
to retry under the same operation key.

Old-name cleanup removes the old topic, then durably writes
`.memory-commit-cleanup.json` in that same directory. Its parent-directory flush
confirms the unlink. A later failure leaves the new topic visible and reports
`Indeterminate`; it does not put the old filename back. CLI keeps its existing
`committed`/`failed` completion vocabulary, but an uncertain receipt carries
`memory.commit.indeterminate` and says that a partial write or unconfirmed result
must not be retried automatically.

## Bounded validation

Save validation retains the existing 8 KiB body and metadata limits. A generated
topic and immutable snapshot have a 16 KiB cap. Topic/catalog input uses the
existing bounded project snapshot (24 MiB topic/catalog aggregate, 4 MiB catalog,
1,024 entries and 4,096 enumerated paths). Preparation scans all project topics;
an existing catalog is validated but does not hide a topic absent from its roster.
Existing-topic fingerprint metadata is validated without reading its referenced
files. Only the new request computes evidence fingerprints. Evidence has a 16 MiB single-file and
64 MiB aggregate cap. Intent and result each have a 128 KiB cap. Missing evidence
retains the existing absent-fingerprint meaning; present but malformed paths or
unreadable/non-regular files fail preparation. Paths are checked and opened as
bounded regular files; ancestor replacement races are outside this contract.

## Acceptance

Remote native tests cover original CLI project upsert completion, typed create
and update inheritance, same-key duplicate and changed-request conflict, invalid
receipts, intent-only crash windows, immutable receipt verification after later
topic updates, lock refusal and cancellation, and failure before/after visible
topic mutation. No local native configure, build or execution is used.
