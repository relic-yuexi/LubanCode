# Public SDK Agentic RAG example

This host supplies a small, owned text corpus through the public `Tool` API. The
SDK runs the model/tool loop, approvals, durable history, cancellation and close.
The model chooses whether to query again and cites sources returned by retrieval.
This example adds no RAG engine, embedding service, vector database or tenant ACL
to LubanCore. The `example://` source URIs identify fictional demonstration
documents; they are not external knowledge-service addresses.

Copy **all four files** to a new directory: this `CMakeLists.txt`, `main.cpp`,
`README.md`, and `examples/sdk-consumer/agentic_rag.cpp`. The copied project only
uses installed LubanCore headers and the STL; it never reads repository sources
at configure/build/run time. Set `CMAKE_PREFIX_PATH` to a compatible installed
LubanCore package. Use matching C++23 compilers, standard libraries, configuration
and Windows CRT. The example works with either Lua build profile.

For an ordinary example build, configure this copied project and build
`lubancore_rag`. Development of this batch does not build locally: its native and
relocated-installed acceptance runs only in remote CI.

Run the deterministic acceptance fixture:

```text
lubancore_rag --fixture ABSOLUTE_STATE_ROOT
```

It executes six real SDK paths: retrieval, sources, preview, isolation, recovery
and lifetime. The fixture model decides follow-up queries from actual tool
replies. Answer text comes from retrieved excerpts with a fresh per-run nonce;
the fixture cannot substitute a prewritten final answer. It tests SDK mechanics,
not a real model's retrieval quality or answer correctness. Same-ID restoration
reopens the saved file session in a fresh Runtime; it is not a cross-process,
external-service recovery guarantee.

To use a real Chat Completions model, put its key in a named environment variable
and pass that variable's **name**, not the secret, on the command line:

```text
lubancore_rag --live ABSOLUTE_STATE_ROOT ABSOLUTE_RESOURCE_ROOT ABSOLUTE_PROJECT_ROOT BASE_URL MODEL API_KEY_ENV "Explain transport and preview from the demo catalog"
```

`BASE_URL` is the provider base expected by the public `Connection` API. Resource
and project roots must already exist. The host reads only the explicitly named
key variable; it does not discover credentials or change process cwd. Confirm
each retrieval request at the terminal. This mode uses the same real text
retriever as the fixture. Replace the host `Retriever` implementation to use your
own corpus or service; Core does not select or authenticate that service for you.

The reference query accepts one JSON `query` field, 1–128 ASCII letters, digits,
spaces or hyphens, and at most 1024 input bytes. The corpus owns at most 16 records:
unique ID (128 bytes), source (256 bytes), and single-line printable ASCII excerpt
(4096 bytes). This deliberately small host codec is not a general JSON, Unicode
or document loader. Actual term matches select at most two owned excerpts, with
a 16 KiB response cap. Empty hits produce insufficient evidence. The citation
check rejects IDs that were not returned; it does not prove entailment or prevent
all model hallucinations.

The Session result policy remains omitted: Preview/v1. Saved evidence shown to
the caller passes through `SavedSnapshot` and `ResultProjector`, with Node full
permission false. This bounds the tool-result wire record; it does not delete
trusted local captures or redact arbitrary model answer text. It is not a data
authorization boundary. Two Sessions inject separate corpus owners even in the
same project. On resume this host supplies the same corpus again; generic custom
tools do not have an SDK-frozen retriever identity or restore an external client.

Retrieval holds no Session, Writer or cancellation flag after a callback. Close
cancels and joins admitted cooperative work before releasing the model and
retriever. A production retriever must also return after cancellation; this host
does not kill arbitrary C++ threads or make network connectivity decisions.
