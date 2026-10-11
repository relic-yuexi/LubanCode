# SDK allocation module qualification

This is a prerequisite of the public Session ordinary-allocation failure matrix,
not that matrix's acceptance. No allocator replacement or refusal is installed.
The existing parser-only probe does not establish Windows SDK DLL ownership.

Testing builds expose a private POD port from the actual shared SDK. It allocates
and releases ordinary scalar, array, aligned and nothrow storage through the
corresponding standard operators. Normal builds and installed public headers
contain no port. The port returns raw qualification storage and function
addresses only; it cannot manufacture Operation, usage, events or recovery facts.

The original native SDK test graphs must resolve both port functions to the
actual CMake SDK image. Exercise SDK-to-SDK, SDK-to-host and host-to-SDK pairs,
unsized and sized release, and zero/small/nontrivial sizes for all eight forms.
Each path must allocate real storage, check aligned addresses where applicable,
write/read its bytes and release through the matching actual module/operator.
Failure assertions still release every allocation. No static SDK clone, host
allocator replacement, allocator-mismatch suppression or synthetic reply proves
these paths. Capture the actual image path and complete native route count.

Require same-source remote three-platform full CLI and SDK Lua ON/OFF, normal
installed consumers and the existing ASan selection. Original native CASEs,
fixtures and checks remain. The subsequent matrix still needs a module-local
ordinary allocator refusal mechanism, full post-Backend-return calibration,
every allocation point, actual Close/retirement and zero-replay same-ID recovery.
Qualification alone does not accept the confirmed completion owner or automatic
Memory. No local configure, build, native tests or repository CI may run.
