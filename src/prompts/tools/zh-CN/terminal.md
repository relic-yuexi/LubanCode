# terminal

## description

交互终端会话,底下是真 PTY(仅 Linux/macOS)。要键盘、要屏幕的程序用它驱动:vim/nano、less、python/sqlite/node REPL、交互式安装向导与问答、ssh、gdb、top。普通一次性命令照旧用 run_command——更快,输出也全。

action:open 起一个 bash 会话(没有 bash 就用 sh),回 session_id 和首屏;send 送按键,等输出落定回屏;read 看当前屏(可等新输出);close 结束会话(SIGHUP、再 SIGTERM、再 SIGKILL);list 列出在册会话。同时最多 8 个,用完就 close。

keys 里的按键记号:<Enter> <Tab> <S-Tab> <Esc> <BS> <Space> <Up> <Down> <Left> <Right> <Home> <End> <PageUp> <PageDown> <Insert> <Delete> <F1>..<F12> <C-a>..<C-z>(<C-c> 中断、<C-d> EOF、<C-z> 挂起)<M-x>(Alt)。不分大小写。字面 "<" 写 <lt>,或者 literal:true 整段原样发。不认识的 <名字> 直接报错,不会当字面敲进去。

结果是当前可见屏幕的纯文本快照(颜色丢掉),头一行标 pid、运行中/已退出(带退出码)、尺寸、光标位置、是否在备用屏(全屏程序)。要看滚出顶端的内容,给 history_lines。

典型流程:open → send "vim notes.txt<Enter>" → send "ihello<Esc>:wq<Enter>" → close。REPL:send "python3<Enter>",再 send "1+1<Enter>",从屏上读答案。send 回来时命令还在跑,就用 read 带 wait_ms 等,别重复送。

## param.action

open / send / read / close / list。

## param.session_id

open 返回的会话编号。send、read、close 必填。

## param.keys

send 要敲的按键:普通文本加 <名字> 记号(见描述);<Enter> 提交一行。

## param.literal

为 true 时 keys 原样发送,不解析 <...> 记号。

## param.wait_ms

回屏前最多等多少毫秒;有新输出且静默 300ms 即提前返回。open/send 默认 1000,read 默认 0,上限 60000。

## param.cwd

open 的工作目录,缺省为当前工作目录。

## param.rows

open 的屏幕行数,默认 40(5..100)。

## param.cols

open 的屏幕列数,默认 160(20..400)。

## param.history_lines

额外附上主屏滚出顶端的最近多少行,默认 0,上限 2000。
