## description

Read a segment of the spilled full text (artifact) of an earlier tool output by stable id: give chunk_id (from a context_search hit) or line_start (1-based) + line_count. At most 32 KiB per call; beyond that the call is rejected with the available range. The full text is verified by sha256; content whose hash does not match is never served.

## param.artifact_id

The aNNNN inside an [artifact aNNNN ...] marker

## param.chunk_id

Chunk id (e.g. c0003); when given, read by chunk

## param.line_start

Starting line (1-based; either this or chunk_id)

## param.line_count

How many lines to read; 0 = read to the end

## summarize_guidance

Set summarize=true only when the artifact is long enough that reading it in segments would cost more. This makes an extra cheap-model call. The summary is appended as this tool result and never rewrites an old message.

## param.summarize

Summarize the whole artifact on demand with the cheap model; costs extra tokens and cannot be combined with chunk_id or a line window
