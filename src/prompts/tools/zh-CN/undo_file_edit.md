## description

撤销此前一次 write_file 或 edit_file 对某个文件的改动。条件式:只有该文件在改动之后没被再改过(当前内容仍与撤销凭据的 postimage 一致)才自动撤销;被改过则不撤销,给出三方对照交人处置。新建的文件在内容未被再改时整枚移走。入参给那次调用的 execution_id(用 /trace 查)。执行前需要用户确认——撤销也是一次写操作,不沿用 write_file 的免问账。

## param.execution_id

要撤销的那次 write_file/edit_file 调用的 execution_id(/trace 可查)
