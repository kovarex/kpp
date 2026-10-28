# PriorityQueue<T, Container, Compare>

A max-heap: `top()` is a greatest element under the comparator Compare (`Less<T>`, which is
`operator<`, unless given; see common.md). Which of several equivalent greatest elements is on top
is unspecified. The elements live in a Container, `Vector<T>` unless given (any container with
`operator[]`, `size()`, `empty()`, `push_back`, `emplace_back` and `pop_back`).

Members: `PriorityQueue()`, `PriorityQueue(compare)`, `PriorityQueue(compare, elements)` (a heap
made from a copy of the container elements, O(n)), `PriorityQueue(first, last)` and
`PriorityQueue(first, last, compare)` (the elements of a range, O(n)), copy, move; `size()`,
`empty()`; `top()` (not empty); `push(value)`, `emplace(args...)`: O(log n) comparisons, amortized
O(1) allocation with a Vector; `pop()`: removes the top element, O(log n); `swap(other)`. Types
`container_type`, `value_compare`, `value_type`, `size_type`, `reference`, `const_reference`.

Push and pop invalidate references to elements. There is no iteration.

Kpp addition: `clear()`.
