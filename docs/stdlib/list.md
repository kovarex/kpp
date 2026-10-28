# List<T>, ForwardList<T>

Linked lists: each element in a node of its own. Inserting and erasing anywhere is O(1) given an
iterator, and never moves or invalidates other elements.

## List (doubly linked)

| operation | iterators, pointers and references |
|---|---|
| any insert, push, emplace, splice | none (spliced elements' stay valid and now belong to the destination) |
| erase, pop | the erased elements' only |
| clear, assignment | all except end |

Members: `List()`, `List({...})`, copy, move; `size()` (O(1)), `empty()`, `front()`, `back()`;
`begin()`, `end()` (bidirectional); `push_back`, `push_front`, `emplace_back`, `emplace_front`,
`pop_back`, `pop_front`; `insert(position, value)` (before position, returns the new element's
position), `emplace(position, args...)`, `erase(position)` (returns the next position),
`erase(first, last)`; `splice(position, other)` and `splice(position, other, it)` (move nodes
from other without copying, O(1) for one element); `remove(value)`, `removeIf(predicate)`;
`reverse()`; `sort()`, `sort(less)` (stable, O(n log n), moves nodes, not elements); `unique()`;
`merge(other)` of two sorted lists (stable); `clear()`.

## ForwardList (singly linked)

As List with one link per node, so only forward iterators and operations at or after a position:
`push_front`, `emplace_front`, `pop_front`, `front()`, `insert_after(position, value)`,
`erase_after(position)`, `before_begin()`, `begin()`, `end()`, `empty()`, `clear()`. There is no
`size()` (it would be O(n)). Invalidation as for List.
