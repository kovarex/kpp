# PriorityQueue<T>

A max-heap: `top()` is a greatest element by `operator<` (with a comparator type, the element
that compares greatest by it). Which of several equivalent greatest elements is on top is
unspecified.

Members: `PriorityQueue()`, copy, move; `size()`, `empty()`; `top()` (not empty);
`push(value)`, `emplace(args...)`: O(log n) comparisons, amortized O(1) allocation; `pop()`:
removes the top element, O(log n); `clear()`.

The elements live in a Vector: push and pop invalidate references to elements. There is no
iteration.
