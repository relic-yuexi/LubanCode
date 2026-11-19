# Returned usage: failure and wire roster correction

Scope: preserve the existing public SDK/CLI five-field ownership contract. This
slice corrects two stale native expectations exposed by same-source remote CI
38098353712. It adds no API and changes no provider bytes or runtime behavior.

The audit Backend already emits MessageStart before returning its tool-only
reply. The bridge commits model.response.started at that point. Arm ordinary
writer failure only after return, then refuse each of the four actual writes:
usage observation, completion event, assistant message, and context admission.
Every refusal must retain the already committed start, preserve exactly the
earlier rows, return failure, and execute zero tools. A fifth post-return write
belongs to tool work and cannot stand in for assistant/context refusal.

Gemini captures returned usage before converting or flushing deferred function
calls. The existing thought/function-call fixture must require UsageSnapshot
before that flush in all four chunk modes. Its wire bytes and non-event manifest
fields remain unchanged. Missing, duplicated or reordered events still fail.

Ownership and retirement remain unchanged. Acceptance requires the original
native CASEs in same-source remote Windows/Linux/macOS full CLI, SDK dual Lua
profiles and installed consumers, plus the existing ASan selection. No local
configure, build, native tests or repository gates may run.
