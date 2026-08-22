## description

对已有文件做字符串替换:先精确匹配,失败后会有限兼容 CRLF/LF、统一缩进和行尾空白。old_string 仍须唯一出现(除非把 replace_all 设成 true),多处候选绝不猜。适合小范围、精准的改动,不适合整篇重写(整篇重写用 write_file)。执行前需要用户确认。

## param.path

要修改的文件路径,相对或绝对均可

## param.old_string

要被替换掉的原文。优先逐字匹配,必要时兼容换行、统一缩进与行尾空白;仍须唯一命中

## param.new_string

替换成的新内容

## param.replace_all

true 就把所有出现的地方都替换掉,不填默认 false(要求唯一命中)
