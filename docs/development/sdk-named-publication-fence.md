# Named publication unknown: live Session execution fence

The exact `e79eea52` installed Linux SDK-only consumer failed after a real named
publication became unknown. The material capability retained its first receipt
and refused further writes, but a fresh operation could still call Generate and
finish successfully without publishing another tool result.

This slice connects the existing first-unknown fact to the live SDK Session.
New Submit calls refuse it; already accepted work stops before dispatch or model
execution; the affected current operation reports Indeterminate and never claims
a complete result. Ordinary terminal/cleanup records may still describe what
actually happened. Cancellation and Close remain cooperative and must retire the
real owners. This is not a rollback of model/tool effects or published bytes.

The capability adds only an internal, allocation-free atomic observation. The
first complete publication receipt remains owned under its existing write gate.
Setting the observation follows retaining that first unknown, before releasing
the material lease. API admission must not copy the receipt or wait for a held
provider/write mutex. Known zero-publication rejection does not set this fence;
ordinary File and confirmed publications keep their existing behavior.

The public installed fixture keeps its original unknown-publication assertion,
CASE roster, markers and budgets. It also checks actual queued work, model-call
counts and a publishing callback that submits through the public Session. The
callback must not deadlock or grant later execution after the owner becomes
unknown. Reads of already saved material and owner retirement remain available.

An admitted command Job may continue after its parent operation has durably
succeeded. If that Job first encounters unknown publication later, the unknown
belongs to the Job and the live storage owner. It must not rewrite the already
confirmed parent final, its complete result or its public query value. New Submit
and queued work still stop, and Close retires the actual command process.

The installed Jobs path proves that ordering with the existing process probe's
release-file argument. It first waits for the real parent Succeeded/complete
result while the command cannot finish. Only then does it arm an after-publication
exception for the next real named write and create the release file. It checks
the resulting Job unknown, unchanged parent facts, refusal of new input, no extra
Generate call, and checked process retirement. No sleep establishes this order;
CASE counts, markers, provider receipt semantics and existing budgets stay fixed.

The same coordinator pump can settle one command and then start an already
queued command. Its scope gate must observe the retained named capability's
atomic unknown flag before dispatch, without waiting for Session module Freeze
after the pump returns. The late fixture therefore holds one running command
and one admitted queued command under the existing single-running limit. After
the first command's publication becomes unknown, the second must be rejected
before process start; its started/done files must remain absent. The successful
parent remains successful throughout.

This first fix is a live-owner fence. A new process reconstructs a new capability;
durably carrying an unknown publication across a crash requires a separately
verified persistent source and recovery contract. This patch must not imply that
reading back orphan bytes or opening a new capability confirms the old receipt.

No public ABI, alternate backend, native receipt, build budget or transport is
added. Validation is source inspection and pure checks locally, then the exact
combined source in remote native CI. No local configure, build, CTest or native
execution is permitted.
