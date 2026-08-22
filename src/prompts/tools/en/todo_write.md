## description

Maintain the todo list for this session with full replacement (every call must pass the complete list, not an incremental update—any item you leave out is gone after this call). Before starting a multi-step task, lay out the list first; after each step is done, mark the matching item's status as completed and pass the full list again so the user can see progress. Passing an empty items array clears the list.

## param.items

The complete todo list, replaced wholesale (not an incremental update; pass the full list every time)

## param.items.content

What this item is to do, stated in one sentence

## param.items.status

The current status of this item
