# Managed Close allocation retirement

This corrects the internal B candidate after ad907275. It does not open public
Managed execution, same-ID recovery, Policy or a second execution stack.

Managed Ledger installs a stack-only retirement guard before constructing its
CloseRequest or fallback diagnostic. It borrows the actual active publication,
writer and Manager rather than copying owner strings. Unwinding attempts checked
writer Close, both write-capability closures and release of the same SessionLock.
It appends no terminal row. A successful regular main Close disarms this guard.

Managed Service installs its own stack-only guard under the existing Close mutex
before allocating diagnostics. On an exceptional exit it seals admission, drains
execution outside the commit mutex, closes the original operation writer under
that mutex, then invokes actual Managed Ledger retirement. Retirement bookkeeping
uses booleans and noexcept moves. It preserves the first append/artifact/checked
close facts and marks the live owner close interruption separately. It does not
turn canonical captured rows into confirmation. Repeated Close after interruption
does not try to write another Ended, cancellation or final row.

The host still stops and joins its turn worker before Close. If async shutdown
cannot establish quiescence, retirement leaves the native lock owned; it does not
pretend to close underneath a running writer. Mutex/system failures and invalid
host lifetime are outside an allocation guarantee. Local Close behavior stays
byte-exact; the new guard and entry are only consumed by Managed closure.
An allocation-free thread-local stack rejects same-owner Close reentry before
waiting on the Close mutex. The rejected entry installs no retirement guard;
the outer owner still controls drain and retirement. Nested other owners retain
the whole stack, so A → B → A cannot hide the original close frame.

New dispatch and new bridge admission still reject the execution-shutdown latch.
An already dispatched operation can publish its real final after that latch while
the same installed owner/main writer remains live, before Managed Close begins.
It still checks the actual immutable dispatched provenance, binding and result
facts. This permits a joined worker's genuine cancelled/success outcome to close
instead of manufacturing an incomplete operation. It starts no additional work;
closing, closed or first-failure owners still reject final publication.

Internal test-only allocation-boundary probes can throw std::bad_alloc immediately
before the real request-reason copy, fallback error assignment, catch diagnostic
and final outcome publication. A capture-boundary throw drives the genuine catch
diagnostic path. These are mechanical exception injections, not allocator or
native-I/O receipts. No probe enters public SDK options. The new native CASE checks
actual originals, two real writer closes, first native Unknown, lock release,
missing or already confirmed Ended and repeat-close behavior. Its execution
evidence awaits remote CI; no local configure, compiler or native run is allowed.

The same patch closes the independently found producer/reader NUL mismatch:
owned final results reject embedded NUL in finalText or error, as their real
producer already does. An actual-source native counterexample rewrites a captured
result as escaped JSON NUL, recomputes the matching result hash/length and canonical
final row, and still requires rejection. No valid payload or native receipt changes.

The original six B CASE bodies remain exact. Two whole CASEs are appended to
`tests/unit/runtime/test_managed_operation_execution.cpp`, with once-only
`close-allocation` and `result-nul` markers under the original prefix. The close
CASE also releases the final real AsyncToolRuntime callback capture during drain:
its nested Shutdown is rejected and nested Close cannot retire the still-open
writer/lock. The outer drain then closes them. No CASE, timeout, workflow or CMake
registration changes here; the root integrates actual registration and remote CI.
That CASE also commits a genuine cancelled final after the shutdown signal and
an error final after the actual Backend signals shutdown during its failing send.
Both read the resulting owned originals and close cleanly; neither admits another
dispatch or bridge. These remain unexecuted native source until remote CI runs.
