# Owned Job queue observation and registration deadline

The Windows SDK-only run of `7a5321c8` passed 61 of 62 focused sources. The
original deadline source passed five of six cases; one check expected `queued`
after parent confirmation but observed `cancelled`. The original six terminal
facts remained present, including no command invocation for this target.

The production registration clock starts immediately before the actual
Registered append. Native registration and parent delivery consume its budget.
ConfirmParentAdmission places the verified delivery in the queue, then pumps
deadlines before returning. A later caller snapshot cannot demand an unexpired
queue if that budget already elapsed. The log has no per-phase timestamps, so it
does not identify which individual write or scheduling interval consumed it.

The fixture will keep the positive 2000 ms registration budget and every
existing terminal, parent-reference, no-Started/no-command, recovery and cleanup
check. An already-cancelled immediate snapshot must have reached the earliest
possible expiry measured from before Register, and the actual terminal must
still say registration_deadline_elapsed. Other states remain errors.

An additional zero-registration-budget ticket must be observed in the real queue
behind the existing live process. Its explicit cancellation and retirement
prove the queue path without racing the positive clock. Zero retains its
existing unlimited registration convention; command caps and the blocker limit
remain unchanged. No mock clock, production change, timeout increase, test skip
or retry is introduced.

Both observations must be printed from actual native execution and checked by
the focused/full/ASan evidence gate. The original source still has six cases and
six path facts. The first failed run stays frozen; only new remote CI can accept
the repair. Local work is source and pure-data checking only.
