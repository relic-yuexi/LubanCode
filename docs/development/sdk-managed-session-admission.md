# Managed new-session storage admission

Base: `37ba5743bb0841b25927c49494ef1c2ded02fce9`. This slice connects existing
Managed reservation to the actual SessionManager/V3 writer. It is an internal
storage prerequisite, not a public Managed SDK, Policy grant or completed ACL.
Old LocalTrusted sessions and their existing opening/resume gates retain their
behavior. Legacy migration and Managed same-ID recovery are separate work.

The only new opening chain is Reserve -> actual PublishOwnership -> Finish ->
Managed SessionManager opening -> V3 initial system metadata. Finish requires this
reservation's original CommittedDurable native publication. The move-only
ManagedSessionDirectory retains the immutable publication, exact expected owner
and bytes, together with its real SessionLock. Only SessionManager can consume
that bundle; there is no public lock Take, receipt setter, directory adoption or
path-only substitute. ExistingMatching and visible bytes after Unconfirmed never
become new-opening authority.

The trusted caller supplies frozen creation audit values: full subject tenant,
user, actor kind and credential ID (an opaque ID, never a bearer secret), plus the
actual opening Policy revision. Ownership already supplies tenant/project,
workspace/session and binding version. These trajectory values record provenance;
they do not authenticate the subject or create another authorization table. The
future SDK boundary must derive them from its existing authorization types and
complete real pre-I/O and execution authorization before exposing Managed runs.

Managed admission checks the real held lock, registered workspace/session path,
unchanged sidecar and original durable receipt before opening subordinate stores.
It checks unchanged ownership again after opening callbacks and before Start.
The first system record includes versioned `managedSession` metadata with mode,
complete owner, binding version, creation subject and opening Policy revision.
The initial metadata is part of actual V3 canonical sequence/hash, not a second
sidecar or a later identity patch. Participant metadata cannot replace it.

Manager has an explicit internal Managed mode. Only new Managed opening and real
Close are enabled for that mode in this slice. Local launch, clear, resume,
workspace recovery and administration cannot silently switch it to LocalTrusted.
Managed new opening consumes the entire bundle. It does not create/reacquire a
second session directory/lock and does not call LocalTrusted new opening first.
The V3 resource/writer assembly is one common private path used by both modes;
the original Local directory creation, ownership gate and failure codes stay in
front of that path. The existing V3 Close seals the writer before releasing lock.

Any failed opening closes candidate writers and subordinate write leases before
releasing the one real lock. Original ownership publication remains immutable;
nonempty ledger or ownership residue is retained. Cleanup can discard only an
owned zero-byte uncommitted stream under the existing cleanup rule. A later
initialization failure is not reported as a failed ownership publication, and
readback must not turn the first native Unknown into success.

Acceptance extends existing Managed reservation native cases without renaming,
removing assertions, adding CASEs or increasing deadlines. It covers actual
Manager lock handoff and Close, original durable receipt preservation, missing or
changed owner/subject/workspace, callback-time drift, opening failure cleanup and
actual first V3 metadata. Existing Unknown/NotCommitted tests, Local new/same-ID
paths and marked-directory reverse gates remain. Exact integrated source runs
native and sanitizer validation only in remote CI; no local configure, compiler,
build, CTest, native process, HTTP, push or PR is permitted for this author slice.
