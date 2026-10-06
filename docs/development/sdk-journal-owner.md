# Main V3 File Journal ownership

This slice gives the main V3 Journal one internal owner for opening, append,
native receipts, capture, same-ID continuation and Close. A new host still calls
the public SDK Runtime: open, submit, wait, close, then open the same Session in a
fresh Runtime and submit again. It does not copy the SessionManager or Writer.

## Boundary

The first adapter is the actual File implementation. Existing JournalWriter and
JournalFileAnchor keep their native I/O; v2 callers retain their current entry
points. V3 owns canonical bytes, sequence numbers, hash chains, system bindings
and projection. The storage owner cannot invent those facts or an authorization
identity. The actual SessionLock remains the admission authority.

No external Journal option is exposed in this slice. SDK feature preparation,
operation restoration and parent proofs still read the main File Journal. Those
reads must converge on the owned opening/read capability before an external
provider can be enabled. No mirror JSONL, fake local path, empty public provider
header, database or network adapter is added to disguise that missing work.

## Lifetime and first uncertainty

The owner holds one write lease. Native append observations stay separate from
V3 completion. A body, newline, flush or sync uncertainty freezes the first
receipt before the append gate is released. Reading, creating another facade,
changing a request key or closing cannot replace it or redispatch the same row.
If native append succeeds and a later in-memory V3 update throws, preserve that
native success and stop further semantic writes; do not relabel it as no write.

Close caches its first real native result and closes the handle once. Readers
own their captured bytes and native File anchor separately; they do not retain
the public Session, Runtime, SessionService or live V3 Writer. File fsync and
FlushFileBuffers do not establish parent or ancestor namespace durability.

## Recovery

Same-ID continuation uses the existing real JournalFileAnchor, full captured
prefix and verified EOF while holding the SessionLock. Keep session/run, bytes,
schema, sequence and chain checks. Reject replacement, truncation and growth;
do not close the anchor and reopen an unrelated path, create a replacement
Session, repair the tail, or repeat model/tool execution.

Child, ancestor, fork/new-ID, legacy adoption/repair and side ledgers remain File
until their own ownership contracts are closed. Future authenticated
ExecutionContext admission and PolicyProvider checks attach before storage
opening; workspace/session scope alone does not grant ACL permission.

## Acceptance

Remote three-platform SDK and installed-host tests must run a complete fresh
Session, Close, new Runtime same-ID recovery and another real turn. Original
Journal-native, V3 receipt, recovery, parent/Post and lifecycle cases must pass.
Additional cases must reach real native uncertainty and continuation rejection,
check one Close, and prove that first receipts cannot be upgraded by readback.
ASan runs remotely too. Local checks inspect source and pure data only.

This contract precedes implementation. It promises main V3 File ownership, not
complete public JournalStore, external storage support or Managed authorization.


## Implemented source boundary, pending remote execution

The main V3 implementation now owns a JournalOwner/FileJournalAdapter rather
than a bare JournalWriter. An AppendLease holds the same stream gate across
actual native append and V3 in-memory completion. Its first NativeAppend or
SemanticCompletion receipt owns row identity, actual canonical-byte length and
the original native observations. A successful native append followed by an
allocation/other exception returns completion-unconfirmed with the real
Committed native receipt, freezes the owner and does not retry. The original
native-only uncertainty accessor remains empty for that semantic failure.

CaptureSessionRecovery takes its main bytes and actual File anchor through the
owner read capability. The locked Manager consumes that same immutable capture
for V3 projection and same-ID continuation. Factories cannot change its main
bytes; continuation retains the original same-object, full-prefix and EOF
checks. Compatibility prefix/anchor entry points and legacy File append remain
available; they do not enable external Journal storage. The active preview
reducer also reads one owner capture instead of its direct path reader.

RecoveryView retains its existing owned-string value interface. The new main
read handle separately owns the same captured bytes. This intentionally retains
one additional main-journal byte copy during opening. Each original SDK
journal/view byte budget still applies to the captured material; it is not a
total resident-memory limit. CLI unset limits retain their original total-size
policy. This slice does not migrate every RecoveryValue caller to shared bytes.
The read handle owns no live Writer, Runtime, SessionService or public Session.

Acceptance source currently contains seven native owner cases, three SDK/STL
host paths and one private actual-public-source guard. The host closes one
Runtime, creates another, restores the same Session and operation identities,
checks zero model/tool replay, and completes another real tool/model turn.
Close-read and same-project multi-session paths exercise separate ownership.
It retains an independently captured old JSONL prefix and a host receipt so CI
can verify those actual artifacts; the running SDK never reads that evidence
copy. Native cases cover all four real append boundaries, semantic completion
exceptions, dropped completion leases, cached actual fclose, native object/
prefix/EOF refusal and actual SessionLock opening. The private guard validates
schema/seq/hash/run and actual prepared-chain projection over the public host.

Only source/data checks have run locally. CMake, install-consumer routing,
workflow classifiers, native source rosters and artifact collection remain for
the integration owner to wire. No local configuration, compilation, native test
or HTTP execution has occurred. These source counts are not remote pass counts.
