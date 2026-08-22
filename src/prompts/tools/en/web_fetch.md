## description

Fetch a web page (HTTP GET, follows redirects). HTML is stripped of tags, leaving the body text; plain text is returned as-is; binary content is not supported. The returned content starts with one line of URL/status/type information. Good for reading documentation and looking things up; when several long pages must be read in depth and summarized, hand the job to an agent subagent instead of piling full-length pages into the main conversation.

## param.url

The full URL to fetch; must start with http:// or https://

## param.max_bytes

Byte cap on the returned body; content beyond it is truncated and marked. Omit for the default 102400 (100KB)
