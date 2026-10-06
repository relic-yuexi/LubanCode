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
- Request preparation records include `modelInputSnapshotScope` and
  `modelInputSnapshot`, plus `outputLimitScope` when a measurement is available.
  The owned snapshot passed to the recorder is the one used by final preflight.
  Action-summary preparation records the same scope and exact measured input.
- Snapshots are values. No pending Generate, writer or Session references are
  held by them. Failed preparation spends no model call and executes no tool.

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
