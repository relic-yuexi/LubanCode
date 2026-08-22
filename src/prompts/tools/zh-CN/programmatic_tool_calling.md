## description

编排一段 Python 脚本批量调用已挂载的只读工具(read_file/search 等):写变量、条件、循环、asyncio.gather 扇出,一段脚本收完把 emit() 的精简摘要送回。适合遍历一批文件、先查 A 再喂 B/C 的长链、同时派多路只读调用后聚合;短任务直接用普通工具更省。输入给 purpose(一句话目的,进审计账)与 script(Python 源码,import luban_tools 拿 typed stubs,结尾必须 emit)。
