## description

Hand a plain-text message to another Lubancode session on the same machine (no files, no chat history—just a short note). Put the peer's name or peer_id in target (list_sessions can look it up). If the peer is busy, the message arrives between two of its tool calls without interrupting the tool it is running; if the peer is idle, it starts a new turn. Send only when the conclusion at hand would affect another live session; no chit-chat, no nagging loops.

## param.target

The name or peer_id of the peer session

## param.text

Plain-text body
