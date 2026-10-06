# Named File paths: owned Session boundary

The exact e79 macOS TSan workflow run failed all five Session-opening cases with
`trajectory.launch_failed: named_result.plan_path_rejected`. No race warning was
present. ReadPlan checked every ancestor, including host directories, before
checking whether a plan existed. Apple installs `/var` as a link to `private/var`;
the failed runner logged `/var/folders/...` but did not identify which ancestor
was rejected. The retained error is an opening failure, not evidence of a race.

LocalTrusted already requires the host to keep Session ancestors stable. Its
ownership capture checks the final directory and opened metadata, and its real
SessionLock checks the locked directory/object pairing. Those ancestors may use
host path aliases. This is not an OS filesystem sandbox, adversarial-parent race
protection, authorization, or a distributed lease.

This slice checks the owned Session directory itself and every component from
that boundary to the requested file. It rejects path escape, a linked/reparse
Session directory, linked intermediate descendants, and linked terminal files,
including dangling links. It never canonicalizes a Session/file before checking
it; doing that would erase the link that the check must reject. Host ancestors
above that boundary retain the existing host-stability contract. The global
secure-file guard, credentials, workspace spelling and fixture inputs stay on
their original paths.

One internal consumed helper applies that boundary to three existing read paths:
the locked Named opening plan; Named File artifact reads; and SDK File artifacts
and saved result policy. The actual Session root, not the artifact parent, is the
boundary. Policy reads use their Session directory explicitly. All original error
codes, pre/post checks, absent-file ordering, provider binding validation, lazy
File opening, immutable native receipts and read budgets remain. No public SDK
API, external Journal option, provider seam or backend dependency is added.

New source acceptance must open real locked Sessions through a host alias, read
actual File results and SDK saved policy, close, then reopen the same Session.
Session boundary/plan/policy/artifacts/result links must still be rejected. The
original five workflow cases, named 10/8 roster, result cases and timeouts remain
unchanged. New native cases supplement them. No fixture may canonicalize its
input to hide the production path behavior. Validation runs in remote native CI;
local work is source inspection and pure checks only.

Runtime::Create retains its existing canonicalization of Runtime roots. The new
acceptance also takes that actual closed Session's verified V3 result index and
same-scope File capability, then calls the real SDK material and policy readers
with the preserved alias spelling. A real TrajectorySessionLedger opening also
uses the alias unchanged. This tests the affected paths without changing Runtime
semantics. Relative roots, a final dot and trailing separators remain usable.

Windows acceptance creates actual directory junctions without symlink privilege.
Root and artifacts cases check junctions. Terminal plan/policy/artifact cases
check a directory reparse object, including a missing target; they do not claim
Windows file-symlink coverage. POSIX terminal cases create actual file symlinks,
including dangling links. The source prints the actual terminal-link kind, and
all six cases remain mandatory. Hard links are not substituted, and no failed
link construction is skipped or counted as a pass.

The independent a803 review found two source blockers. MSVC absolute(path) calls
GetFullPathNameW even for absolute input, so it can erase an owned descendant `..`
before the guard sees it. Also a temp directory can live on a different drive
from CTest's working directory; lexically_relative then correctly returns empty.
The a803 source and blocked review remain retained, not upgraded by this repair.

The repair keeps absolute inputs verbatim. Ordinary relative inputs share one
captured current_path. On Windows only a drive root_name is passed to absolute
to resolve that drive's current directory; repeated use of the same drive shares
that result. The raw relative_path is appended afterward. Root-relative input
uses the chosen base.root_path. UNC/device absolute input is never split by hand.
No complete user path is passed through absolute/canonical/lexical normalization
before the domain check. Relative dot and trailing separators retain their old
meaning, and raw owned-descendant `..` remains visible and rejected.

The relative standalone test gets its own uniquely named ordinary directory
under the actual CTest working root. It verifies that exact absolute child before
creating or cleaning it, passes a genuinely relative path to ResultStore and
reads the actual native bytes. It neither switches cwd nor substitutes an
absolute call or skip. The six case/marker roster and original alias/native
counterexamples remain. The source adds real same-drive drive-relative and
root-relative checks on Windows; it does not claim I/O on invented UNC hosts.
