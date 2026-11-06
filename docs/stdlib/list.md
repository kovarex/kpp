# List<T>, ForwardList<T>

Linked lists: each element in a node of its own. Inserting and erasing anywhere is O(1) given an
iterator, and never moves or invalidates other elements.

## List (doubly linked)

| operation | iterators, pointers and references |
|---|---|
| any insert, push, emplace, splice | none (spliced elements' stay valid and now belong to the destination) |
| erase, pop | the erased elements' only |
| clear, assignment | all except end |

Members: `List()`, `List({...})`, `List(count)`, `List(count, value)`, `List(first, last)` (input
iterators), copy, move, `= {...}` (replaces the contents with the listed elements);
`assign(first, last)`, `assign(count, value)`, `assign({...})` (replace the contents, assigning over
the elements there are, as copy assignment and `= {...}` do); `resize(n)`, `resize(n, value)`;
`size()` (O(1)), `empty()`, `front()`, `back()`;
`begin()`, `end()` (bidirectional), `rbegin()`, `rend()`, `swap(other)`; `push_back`, `push_front`, `emplace_back`, `emplace_front`,
`pop_back`, `pop_front`; `insert(position, value)` (before position, returns the new element's
position), `insert(position, count, value)`, `insert(position, first, last)`, `insert(position, {...})`
(returning the first one's position, or position for none), `emplace(position, args...)`, `erase(position)` (returns the next position),
`erase(first, last)`; `splice(position, other)`, `splice(position, other, it)` and `splice(position,
other, first, last)` (move nodes from other without copying: O(1), a range of another list O(its
length); other may be an rvalue); `remove(value)`, `removeIf(predicate)`;
`reverse()`; `sort()`, `sort(less)` (stable, O(n log n), moves nodes, not elements); `unique()`,
`unique(same)`; `merge(other)`, `merge(other, less)` of two sorted lists (stable; other may be an rvalue); `clear()`; `==` and `<=>` (common.md).

## ForwardList (singly linked)

As List with one link per node, so only forward iterators and operations at or after a position:
`push_front`, `emplace_front`, `pop_front`, `front()`, `insert_after(position, value)` and
`emplace_after(position, args...)` (returning the new element's position), `erase_after(position)`
and `erase_after(first, last)` (removing what is strictly between; returning the position after
the removed ones), `before_begin()` (the position before the first element, for inserting at the
front), `begin()`, `end()`, `empty()`, `remove(value)`, `removeIf(predicate)`, `reverse()`, `sort()`,
`sort(less)` (stable, relinking the nodes), `clear()`, `= {...}`, `==` and `<=>`.
There is no `size()` (it would be O(n)). Invalidation as for List.
