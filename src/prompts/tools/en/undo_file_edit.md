## description

Undo a previous write_file or edit_file change to a file. Conditional: the file is restored only if it has not been modified since (current content still matches the postimage recorded with the undo credential); otherwise no automatic undo happens and a three-way comparison is returned for a human decision. A newly created file is removed when its content is unchanged. Takes the execution_id of that call (find it via /trace). Requires user confirmation — an undo is itself a write, and a write_file accept-for-session does not carry over.

## param.execution_id

The execution_id of the write_file/edit_file call to undo (see /trace)
