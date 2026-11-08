# Deque<T>, Queue<T>, Stack<T>

## Deque

A sequence with O(1) access by index and O(1) insertion and removal at both ends. Elements are
stored in fixed-size blocks; adding at either end never moves the existing elements.

| operation | iterators | pointers and references |
|---|---|---|
| push_back, push_front, emplace_back, emplace_front | all | none |
| pop_back | the last element's, and end | the last element's |
| pop_front | the first element's | the first element's |
| insert or erase in the middle | all | all |
| clear | all | all |

Complexity: `operator[]`, `front`, `back`, push and pop at either end: O(1). Insert or erase at
position i: O(min(i, size() - i)); erasing n elements at i: O(n + min(i, size() - i - n)).

Iterators stay on their element through removals at the ends (std::deque's rule) and through `swap`
and moves, which hand the elements over.

Members: `Deque()`, `Deque({...})`, `Deque(count)`, `Deque(count, value)`, `Deque(first, last)` (the
elements of a range of input iterators, in order), copy, move, `= {...}` (replaces the contents with the listed elements); `size()`, `empty()`; `operator[](i)`, `at(i)`
(throws OutOfRange), `front()`, `back()`; `begin()`, `end()` (random access iterators);
`rbegin()`, `rend()`, `swap(other)`;
`push_back`, `push_front`, `emplace_back`, `emplace_front`, `pop_back`, `pop_front` (not empty);
`assign(first, last)`, `assign(count, value)`, `assign({...})` (replace the contents, assigning over
the elements there are, as copy assignment and `= {...}` do; the range must not be this deque's); `insert(position, value)`, `insert(position, count, value)`, `insert(position, first, last)` (input
iterators not into this deque), `insert(position, {...})` (the listed elements in order; the shorter
side moves by the count, by move construction and move assignment, as std::deque's; at either end
nothing moves, so pointers and references stay; O(count + min(i, size() - i))),
`emplace(position, args...)` (the element made
from args, then as insert) and `erase(position)` returning the position of the inserted element /
the element after the erased one; `erase(first, last)`, removing the elements in [first, last) and
returning the position of the element after them; `resize(n)`, `resize(n, value)`, `clear()`; `shrink_to_fit()` (does
nothing: a block is freed when removals empty it, and elements never move); `==` and `<=>`
(common.md).

When an element's constructor or assignment throws, the deque stays whole, as std::deque's does: it
counts no element it did not build, and what it built before the throw is destroyed again (the basic
guarantee); a push, and an insert of one element, of a count of one value or of a forward range at
either end leave the elements as they were (the strong one; std promises it for one element). A
constructor that throws destroys the elements it built and frees its blocks
(library/deque_stays_whole_when_an_element_throws).

## Queue

`Queue<T, Container>`: first in, first out, over a Container (`Deque<T>` unless given):
`push(value)` / `emplace(args...)` at the back, `pop()` at the front, `front()`, `back()`,
`size()`, `empty()`, `clear()`. Same invalidation and complexity as the Deque operations they use.
Types `container_type`, `value_type`, `size_type`, `reference`, `const_reference`.

## Stack

`Stack<T, Container>`: last in, first out, over a Container (`Deque<T>` unless given):
`push(value)` / `emplace(args...)`, `pop()`, `top()`, `size()`, `empty()`. References to elements
stay valid while other elements are pushed and popped. Member types as Queue's.
