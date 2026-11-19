# Workflow Session opening failure evidence

Source `68e1fcf2` executed the TSan workflow suite. Its five node-session cases
all stopped at `opened.has_value()`. The shared helper discarded the actual
`TrajectorySessionLedger::Open` error. The log therefore establishes failed
opening, not a race or a unique storage/path cause.

On that failure branch only, print the real error and the root, workspace and
workspace-storage paths actually passed to opening. Keep returning `nullopt`.
Keep all five cases, their assertions, existing inputs, cleanup and timeout.
Do not retry, substitute a directory, or convert failed opening into a pass.

No new resource or SDK API is added. Existing fixtures own their temporary
directories and sessions. The next remote run must retain this diagnostic if
opening fails. Success still requires the original workflow assertions.
