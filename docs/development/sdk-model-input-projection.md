# SDK model input measurement

This slice closes the Generate-only SDK backend's missing request measurement.
An SDK tool batch must be able to plan, produce and adopt a real action summary
without raising its host turn budget or replacing the material sent to Generate.

## Contract

- Public `lubancore::Backend` remains Generate-only. This adds no public virtual
  method, provider protocol, network transport or installed dependency.
- Internal `api::Backend::PrepareModelInput` returns three states: an owned
  snapshot, unavailable, or an error. The default selects model input from the
  existing provider serialization. Empty serialization stays unavailable; an
  invalid nonempty serialization stays an error. Legacy trace backends retain
  their old preflight path.
- Every snapshot declares its source scope. `provider_wire_input_v1` means the
  selected input fields in the provider's final serialized request.
  `sdk_model_request_v1` means the exact system, ordered Message values and Tool
  definitions handed to public Generate. It says nothing about a custom backend's
  later prompt rewriting, provider wire, hidden context or actual tokenizer.
- SDK sending and measurement share one checked conversion. Tool arguments and
  tool schemas remain JSON strings. Text aggregation, message roles, tool IDs,
  reply text and error flags match Generate exactly. Unsupported rich or
  structured history returns the same error before Generate; it never falls back
  to an unmeasured successful send.
- Model selection and output limits are control data, excluded from input bytes.
  The existing ceil(UTF-8 compact JSON bytes / 4) estimator remains a text proxy.
  Source scope is metadata outside the measured input object. SDK output-limit
  facts refer to Generate's requested `max_output_tokens`, never an observed
  provider cap. Missing limits stay missing; existing conservative reserves stay.
- Main preflight, context pressure, batch shell planning, adopted batch checking
  and action-summary preflight consume this same internal capability. Summary
  exact system/material checks, no-tool rule, output reserve, window/margin,
  call/depth budget, durable source verification and adoption gates stay in force.
- Spinner and Rebuildable wrappers forward the capability. Four production
  providers continue through their existing final serialization and extra_body
  selectors. SDK SerializeForDiagnostics, PreparedWireRequest and wire-message
  map remain unavailable. No provider-message mapping is fabricated.
- Main request preparation records include `modelInputSnapshotScope`, snapshot
  SHA256/UTF-8 bytes and the explicit `sha256-compact_json_utf8_v1` fingerprint
  algorithm, plus `outputLimitScope` when a measurement is available. The owned
  snapshot passed to the recorder is the one used by final preflight. This digest
  is evidence only; it does not reconstruct input or replace the original
  system/inputMessageRefs/tool sources. Full history is not copied into each
  main prepared event. Action-summary preparation retains its existing
  `modelInputSnapshot` and records the same scope and exact measured input.
- Snapshots are values. No pending Generate, writer or Session references are
  held by them. Failed preparation spends no model call and executes no tool.

## Boundaries

This input view does not certify a custom backend's provider transformations or
its compliance with a requested output cap. Such transport facts need a later
optional provider capability; this slice neither adds one nor infers it.
Rebuildable snapshots one inner backend per capability call, as before. It does
not pin one generation across measurement and later sending; this slice does not
claim to close the existing cross-call configuration-generation contract.
The SDK's per-Session adapter holds its backend throughout the session.

The input JSON schema is `system`, ordered `messages` and `tools`. Every message
has `role`, concatenated `text`, ordered `tool_calls` (`id`, `name`, `input_json`)
and ordered `tool_replies` (`call_id`, `text`, `is_error`). Every tool has `name`,
`description`, `input_schema_json`. Empty vectors are arrays. The two JSON-valued
SDK string fields remain strings; input counting includes their actual escaping.
Main prepared adds `modelInputSnapshotSha256`, `modelInputSnapshotUtf8Bytes` and
`modelInputSnapshotFingerprintAlgorithm`; the scope fields sit beside them.

## Source validation inventory

- `test_lubancore_model_input.cpp`: six native cases and six distinct markers.
  Exact Generate values; unsupported/cancelled inputs; legacy/four-provider
  selection; Rebuildable plus the real loop's Hook/prepared frozen view; real
  summary preparation/adoption; fail-closed summary counterexamples.
- `test_model_input_wrappers.cpp`: one CLI native case and one marker. Single
  Spinner and nested Spinner/Rebuildable each forward all three outcomes.
- Existing named-results public helper keeps its ten cases and original host
  budget. Its Generate fixture now checks the requested 1024-token cap and exact
  tool-free single-user shape. Native inspection independently reads committed
  summary system/prompt sources, recomputes input bytes, checks measurement scope
  and the subsequent main prepared evidence fingerprint.

These are source assertions awaiting remote CI. No native result is inferred
from their existence or from a Python fixture.

## Verification plan

Remote native checks must cover exact SDK Generate/projection equality, role and
tool/reply preservation, quoted/control/unicode bytes, schema/argument strings
that resemble media, rejected rich/structured history, unavailable vs invalid
legacy material, all four provider defaults and wrapper forwarding. A bounded
summary must prove an actual Generate call sees the exact system/material and
requested output limit, then prove scoped preparation and durable adoption.
Counterexamples must stop before Generate when the input is oversized, replaced,
contains tools, or has unknown/too-large output limits. Existing installed
named-results summary remains an independent public SDK acceptance path.

No local configure, build, native test or HTTP execution is permitted. Source
review is not CI evidence. CMake and CI registration are wired separately by the
integration owner, after this source is sealed.
