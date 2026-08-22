## description

维护本次会话的待办清单,整表替换(每次调用都要传完整的清单,不是增量更新——漏掉的项这次就没了)。多步骤任务开工前先列一份清单,每完成一步就把对应项的 status 改成 completed 再整表传一次,让用户能看到进度。items 传空数组表示清空清单。

## param.items

完整的待办清单,整表替换(不是增量更新,每次都传全量列表)

## param.items.content

这一项要做的事,一句话说清楚

## param.items.status

这一项当前的状态
