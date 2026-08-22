## description

Work inside an isolated git worktree and leave the main checkout untouched. For big changes, start with worktree enter (a default name is generated when omitted; base fresh=the remote default branch, or head=the current HEAD); the whole session moves into the room—reads, writes and commands all happen inside, and the status line shows the room name. When done, worktree exit keep (keep the room) or exit remove (deleted only if clean; a dirty room needs user confirmation). worktree status tells whether you are in a room and whether it is dirty; worktree list lists every worktree. Do not commit build artifacts into the room; changes made in the room must eventually be merged back into the main branch.

## param.action

enter=create a room or enter an existing one (the whole session moves in); status=room state; list=list worktrees; exit=move back (pair with mode)

## param.name

Room name at enter time (alphanumerics, dash, underscore); generated automatically when omitted. You may also pass the name or path of an existing worktree; rooms outside the garden (.lubancode/worktrees) require user confirmation first

## param.base

Base for creating a new room at enter: fresh=the remote default branch (default; the fetch is capped at 5 seconds and falls back to local on failure); head=the current HEAD

## param.mode

How to exit: keep=the room stays on disk; remove=deleted only if clean (a dirty room must have the user's consent—never nod on the user's behalf)
