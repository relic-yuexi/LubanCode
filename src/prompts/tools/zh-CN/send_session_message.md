## description

给同一台机器上另一场 Lubancode 会话递一条纯文本消息(不传文件、不传聊天记录,只递一张字条)。target 填对方的名字或 peer_id(list_sessions 可查)。对端正忙时消息会在两次工具调用之间送达,不打断它手头的工具;对端空闲则另起一轮。只有在手头结论会影响另一场活会话时才发送;不许闲聊,不许催问成环。

## param.target

对方会话的名字或 peer_id

## param.text

纯文本正文
