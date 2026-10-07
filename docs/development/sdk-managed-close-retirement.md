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

Internal test-only allocation-boundary probes can throw std::bad_alloc immediately
before the real request-reason copy, fallback error assignment, catch diagnostic
and final outcome publication. A capture-boundary throw drives the genuine catch
diagnostic path. These are mechanical exception injections, not allocator or
native-I/O receipts. No probe enters public SDK options. The new native CASE checks
actual originals, two real writer closes, first native Unknown, lock release,
missing or already confirmed Ended and repeat-close behavior. Its execution
evidence awaits remote CI; no local configure, compiler or native run is allowed.
