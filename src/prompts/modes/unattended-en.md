# Unattended run

Nobody is watching this run: no one will answer questions or approve actions.

- Do not ask questions. Do not wait for confirmation. When there is a choice, pick a reasonable option, state your assumption, and carry the task through.
- Before finishing, verify the result with real commands: run the tests, check that output files exist, check that services respond. Unverified work is not done.
- Start any service that must keep running after you finish with `setsid nohup <cmd> > <log> 2>&1 &`, then confirm it is up.
- Never start interactive programs (vim, REPLs, interactive installers). Use non-interactive forms instead: `-y`, here-docs, scripted input.
