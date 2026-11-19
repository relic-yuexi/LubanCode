# SDK Prepare Journal capture

This private slice starts at `1625765b`. Action, Command Jobs, Subagent, Skills
and Lua keep their existing Prepare read points and error handling, but the SDK
opening supplies one lazy File read owner. Its first successful actual capture
supplies each module's verified main ledger; a later module never silently reads
a different body. A direct private module caller without that owner retains the
old File reader. No public SDK/provider interface is added.

Core Prepare order remains Action, Jobs, Subagent, Skills, Memory, Memory Write,
Lua. The separate locked opening order remains Action, Jobs, Skills, Memory,
Memory Write, Subagent, Lua. Action's soft probe remains soft; true missing-scene
errors and each module's own plan/binding errors stay at their original phase.
No resource factory, MCP process, Backend call or Lua load moves earlier.

The lazy owner captures through the real JournalOwner/File anchor and original
V3 verification. It closes the anchor before returning values. An existing
verification failure wins over cleanup exceptions; successful verification still
requires checked Close. Capture/verification errors stay cached for this opening,
without converting an ignored soft probe into a fabricated successful witness.
The original unbounded preflight File policy remains; later locked recovery keeps
its existing finite limits. Five modules share a const ledger view, but captured
bytes, parsed lines and the later locked ledger can coexist; no aggregate
resident-memory guarantee is claimed.

Only a successfully verified capture produces an internal witness: actual
expected workspace/session/main resource scope, captured byte count and SHA256.
The witness does not certify that the ledger claims this Session: the original
module and locked-manager scope checks retain that responsibility and error
priority, including a valid foreign main stream. It is carried by
the existing private recovery request. CaptureSessionRecovery compares it against
its own actual MainV3 reference bytes after original reference validation and
before any existing recovery factory. The factory cannot replace those bytes.
A mismatch refuses resume as source_changed. Neither the preflight ledger nor
the witness can replace the locked capture, actual native anchor or the original
full-prefix/EOF checks used by ContinueCaptured.

The existing SessionLock remains in SessionManager. OpenLockedMemory still runs
before the locked capture; this patch does not claim to eliminate that earlier
provider window. Backend/MCP assembly still follows all original Prepare checks.
Its existing RAII cleanup and Runtime pending-opening lifetime remain in force.
Only the small witness crosses into retained runtime options; preflight material
does not retain a Writer, SessionService, public Session or long-lived callback.

Real-source tests extend existing cases: successful public same-ID reopening,
unchanged module error priority, a captured body followed by actual source drift
before locked recovery, and proper resource retirement. Case counts, markers and
budgets remain fixed. A source counter alone is not native acceptance evidence.
Future Managed admission must authenticate before this private read owner; a
LocalTrusted root, SessionLock or digest is not an ACL or a public grant.

No local configure/build/CTest/native/HTTP execution, CI edits, push or new PR.
Source review and pure checks precede separate combined-source remote CI.
