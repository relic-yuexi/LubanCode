## description

Queue a small, stable project fact, an explicit user preference, or a user-stated correction of how things are done into background memory (a formal write, bypassing the review queue). Call it only when the information has been confirmed by source code, tool results, or the user's own words; a fact must carry verifiable evidence in paths or evidence; feedback only accepts corrections the user stated on the spot (release cadence, acceptance habits), with confidence set to user-stated—model inference must never write directly. Do not store current task progress, guesses, logs, raw web/MCP text, secrets or personal data. When the same topic already exists, reuse the id from the index to update it. Automatic candidates go through turn summaries, not this tool.

## param.kind

fact=a verifiable project fact; preference=a project preference the user stated explicitly; feedback=a user-stated correction of how things are done (must be user-stated)

## param.id

Optional. The stable id from the index, when updating an existing memory

## param.title

A short topic that can be updated independently

## param.summary

One-line summary for the index

## param.content

Distilled body; write facts, evidence and caveats, do not copy long stretches of source

## param.keywords

Exact retrieval terms—function names, class names, commands; at most 16 items

## param.paths

Project-relative paths supporting the fact; at most 24 items; a fact requires at least one

## param.confidence

user-stated=a preference the user stated; verified=a verified fact; inferred=inference (belongs only in review-queue candidates, never through this tool)

## param.scope

Optional. Not injected when the current working directory is out of scope; keeps flavors from bleeding together

## param.scope.kind

Scope the memory applies to; subtree/path must pair with value; user=cross-project user memory (preference/feedback only, must not carry project path evidence, requires the global authorization memory.user_enabled)

## param.scope.value

Project-relative path (required for subtree/path)

## param.evidence

Optional. Verifiable evidence; at most 24 items; recommended for facts

## param.evidence.path

Project-relative path

## param.evidence.symbol

Optional: function/class/config key

## param.expires_at

Optional. Expiry date of a temporary rule (YYYY-MM-DD or an ISO timestamp); no longer recalled after expiry

## param.occurred_at

Optional. When the fact happened (YYYY-MM-DD or an ISO timestamp). Only fill it in when the material states it explicitly; omit it when no date is given, never guess
