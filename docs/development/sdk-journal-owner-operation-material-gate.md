# Journal owner ordinary-operation material gate

## Contract before implementation

The first `e79eea52` installed Linux Journal consumer completed all three public
paths. Its saved main ledgers contain no `sdk.operation.turn.bound`. That event
comes from the explicit Jobs producer; an ordinary SDK host takes the existing
`PopPendingInput` path. The evidence checker must not enable that producer or
change SDK input, cancellation, error, or write semantics to satisfy its gate.

Capture the actual `operations.jsonl`, `operations-inputs/<operation>.json` and
`sdk-results/<operation>.json` beside the already captured main ledger, old
prefix, host receipt and raw/formal artifacts. Keep the same bounded entry,
per-file and total-byte limits, reject links/path escapes, preserve partial
captures as `not_evaluated`, and read only retained bytes during acceptance.

For this ordinary-host fixture, verify exactly one accepted -> dispatched ->
successful final sequence per operation. Bind the host's OLD/NEW operation IDs
to distinct input IDs and turn IDs, the actual input reference and text SHA-256,
one SDK/local-user input event and message, the final assistant reference and
SDK result artifact. Verify the same sequence for the separate OTHER Session.
Reject missing, duplicate, extra, reordered, foreign or incomplete material.
The text hash is the producer's no-image input payload rule, not a Python
reconstruction of C++ canonical JSON. No tenant/actor fields are invented;
the actual `senderKind=local_user` and `source=sdk` must remain unchanged.

Keep the existing public three-path/4-model/2-tool, zero replay, closed-owner,
prefix, run/sequence/hash-linkage, raw/formal metadata/combined bytes and
selection checks. C++ native guards still own canonical JSON and prepared-chain
validation. Pure fixtures test the checker only; they do not certify native
execution. The original first-failure archive stays unchanged. Its missing
operation files must remain a rejection, never be synthesized from stdout.

Only the checker and its pure tests change after this contract. No product,
public header, native helper, CMake, timeout, workflow or live-read migration is
part of this slice. A new remote run must retain the new original files before
the revised material gate can claim acceptance.
