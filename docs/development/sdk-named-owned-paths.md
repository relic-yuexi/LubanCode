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
