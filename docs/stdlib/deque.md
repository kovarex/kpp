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

Members: `Deque()`, `Deque({...})`, copy, move; `size()`, `empty()`; `operator[](i)`, `at(i)`
(throws OutOfRange), `front()`, `back()`; `begin()`, `end()` (random access iterators);
`rbegin()`, `rend()`, `swap(other)`;
`push_back`, `push_front`, `emplace_back`, `emplace_front`, `pop_back`, `pop_front` (not empty);
`insert(position, value)` and `erase(position)` returning the position of the inserted element /
the element after the erased one; `erase(first, last)`, removing the elements in [first, last) and
returning the position of the element after them; `resize(n)`, `clear()`.

## Queue

`Queue<T, Container>`: first in, first out, over a Container (`Deque<T>` unless given):
`push(value)` / `emplace(args...)` at the back, `pop()` at the front, `front()`, `back()`,
`size()`, `empty()`, `clear()`. Same invalidation and complexity as the Deque operations they use.
Types `container_type`, `value_type`, `size_type`, `reference`, `const_reference`.

## Stack

`Stack<T, Container>`: last in, first out, over a Container (`Deque<T>` unless given):
`push(value)` / `emplace(args...)`, `pop()`, `top()`, `size()`, `empty()`. References to elements
stay valid while other elements are pushed and popped. Member types as Queue's.
