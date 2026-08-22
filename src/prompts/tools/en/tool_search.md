## description

Search deferred-mounted tools by keyword (MCP/plugin and other external tools do not go straight into the tool table; they only show their names in the index segment of the system prompt). Performs case-insensitive token matching over tool names and descriptions; matched tools are mounted immediately and can be called directly from the next turn on. Use this first when the index segment lists a capability you need, or when you suspect an external tool can do the job.

## param.query

Keywords, multiple words separated by spaces; case-insensitive substring matching over deferred tools' names and descriptions, ranked by the number of tokens hit.

## param.limit

How many to return and mount at most; omit for the default 5.
