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


## Active write lease lifetime correction

An issued AppendLease now independently holds the actual shared owner State,
locked mutex and File adapter. Moving, replacing or destroying the facade cannot
retire that native stream while its lease is active. The lease freezes its first
semantic uncertainty while still holding the gate, unlocks the live mutex, then
releases its State reference. Final File retirement closes the original native
handle once. Repeated explicit Close retains its first fixed native receipt.
There is no State-to-lease reference or owner/read-handle cycle; independent read
handles continue to own only captured bytes and their native File anchor.

The existing seven-case native roster additionally exercises an issued lease
outliving its owner scope, moved and replaced owners with an active lease, and
actual concurrent Close waiting for that lease to exit. The concurrent case
keeps actual Committed native append separate from SemanticCompletion unknown,
then preserves the first real injected-unconfirmed fclose observation. It uses
zero-duration future polls, adds no timeout allowance and does not relax any old
native case. These additions remain source-only pending remote CI.

## Public host raw/formal acceptance correction

One plain host tool invocation persists two selected SDK records: its raw
`capture-*` material and formal `res-*` result. The installed SDK/STL host now
groups every indexed record by the actual `(tool_call_id, attempt)`, requires one
group with exactly one raw and one formal record, and uses that formal material
as the representative result. It never assumes a one-record list or takes an
unclassified first/last result.

Both records must match the actual Session, completed operation, turn and first
attempt; persisted-event/result identities must be distinct. Their verified
metadata, execution event and one verified UTF-8 combined channel must retain
the host callback's exact text, byte counts, capture facts and artifact identity.
The default Preview/v1 policy must remain bound to that Session. Raw and formal
records share the actual execution and output digest; they remain two durable
records for one callback.

Same-ID recovery lists and verifies both records again, then compares each
original six-part identity, metadata digest/bytes, execution event, result kind,
policy and every combined-channel field. Closed readback checks both original
records before and after the new Runtime closes. The new turn verifies its own
raw/formal pair and fresh turn/action/execution identities. All three public
paths and the private actual-public guard invoke this same helper; the 7/3/1
case roster, model/tool counts, zero-replay, old JSONL prefix and weak-owner
checks remain. The retained host receipt additionally records both OLD/NEW
raw/formal identity and digest/byte witnesses. This correction is source-only;
no earlier CI run establishes its execution result.

## Integration acceptance wiring

The actual engine compiles JournalOwner once. SDK reference and full CLI test
targets compile the same public SDK/STL helper. The installed and relocated
consumer takes only its explicit state root. Lua ON/OFF keeps this acceptance
available; no internal Writer header enters the installed helper.

The source roster adds three focused registrations and eleven CASE: seven
internal owner cases, three public host cases and one actual-main guard. The
same full run retains these three original registrations and their focused
counterparts. Its existing unit timeout stays 180 seconds; the other five
registrations stay at 300 seconds. The installed consumer stays at 120 seconds.

Even a failed consumer retains bounded real fixture materials before parsing
acceptance. Collection starts as not_evaluated. Successful checks bind the
original host argv, Session and operation identities, old main prefix and
raw/formal file digests. The Python checker does not claim C++ canonical hashing;
the original native guard owns that check. ASan retains the old source roster
and PCH checks and adds these three sources plus the actual public helper.

Only source, pure-data and document checks have run locally. Remote execution
of this combined source is still pending.
