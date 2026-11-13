# Owned deadline assertion grouping

The ASan build for source `68e1fcf2` failed before native execution. Doctest
decomposed a bare `||` in the existing queued-or-cancelled assertion and rejected
the expression. The retained compiler log identifies that assertion directly.

Wrap the whole Boolean expression in parentheses. Keep both accepted states,
the 2000 ms registration budget, the 1999 ms expiry floor, all six cases, and
every native receipt check. This changes macro grouping, not the assertion.

The test owns no new resource. The existing fixture closes jobs and sessions.
The next remote ASan build and native run must validate the repair; source
review alone does not count as a passing ASan result.
