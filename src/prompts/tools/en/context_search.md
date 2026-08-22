## description

Search the spilled full text of earlier tool outputs (artifacts) by keyword. When a tool result is too long, the request keeps only an [artifact aNNNN ...] reference (head/tail preview); when the preview is not enough, use this tool to search the full text, get the hit line numbers and chunk ids, then use context_read to pull out the context. Never mistake the preview's ellipsis for the full text.

## param.artifact_id

The aNNNN inside an [artifact aNNNN ...] marker

## param.query

Keyword (ASCII case-insensitive; Chinese matched as written)

## param.max_results

Maximum number of hits to return (default 8)
