## description

住进隔离的 git worktree 里干活,不碰主 checkout。大改动先 worktree enter(缺省名字自动生成,基准 fresh=远端默认分支或 head=当前 HEAD),整场会话搬进房里:读写、命令都在房内,状态行会亮房名;干完 worktree exit keep(留房)或 exit remove(干净才删,脏了要用户确认)。worktree status 看在不在房里、脏没脏;worktree list 列全部工作树。别把构建产物提交进房里;房里的改动最终仍要合回主分支。

## param.action

enter=建房或进已有房(整场会话搬进去);status=房内状态;list=列工作树;exit=搬回原处(配 mode)

## param.name

enter 时的房名(字母数字-_),不填自动生成;也可传已有 worktree 的名字或路径,园子(.lubancode/worktrees)之外的房要先经用户确认

## param.base

enter 建新房的基准:fresh=远端默认分支(缺省,fetch 5 秒封顶失败回落本地);head=当前 HEAD

## param.mode

exit 的方式:keep=房留在盘上;remove=干净才删(脏了必须用户确认,别替用户点头)
