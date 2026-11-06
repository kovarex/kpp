# Span<T, Extent>

What `std::span` converts to: a view of contiguous elements, a pointer and a count. A Span does
not own its elements; they must outlive it, and adding to or removing from the Vector it views
invalidates it as it invalidates the Vector's pointers. `Span<const T>` views elements it may not
change.

`Extent` is the count when it is fixed (`std::span<T, 3>` is `Span<T, 3>`), or `dynamicExtent`
(std::dynamic_extent, the default) for a span whose count is known only when the program runs.
`Span<T, N>::extent` is N.

Members:
- `Span()`: views nothing (a fixed extent's count stays its extent).
- `Span(T* items, ContainerSize count)`, `Span(T* first, T* last)`: those elements.
- From a Vector, an Array, a plain array (`T[N]`) or a Subrange of pointers whose elements convert
  to T as pointers (U to const U, a derived class to a base): implicit for a span of any length;
  for a fixed extent, explicit from a Vector or a Subrange (whose count must be the extent), and
  only from an Array or a plain array of that count.
- From another Span (of U to const U, or of another extent): implicit when the count is sure to
  fit (to any length, or between the same fixed extent), explicit from a span of any length to a
  fixed extent.
- Deduction, as std::span's guides: `Span(vector)` is a `Span<T>` (`Span<const T>` of a const
  Vector), `Span(array)` of an Array or a plain array a `Span<T, N>`, `Span(p, count)` and
  `Span(first, last)` a `Span<T>`.
- `size()`, `size_bytes()`, `empty()`; `operator[](i)`, `data()`, `begin()`, `end()` (pointers),
  `rbegin()`, `rend()` (from the last element to the first), `front()`, `back()`. Accessing an
  element out of range is a program error, not checked.
- `subspan(offset, count)`, `subspan(offset)`, `first(count)`, `last(count)`: Spans of any length
  over a part; a count of `dynamicExtent` is the rest from offset, as `subspan(offset)`.
- `element_type`, `value_type`: T; `size_type`: ContainerSize; `iterator`, `pointer`: T*.

*Differences from C++*: the count is a ContainerSize (common.md); a Span of a fixed extent stores its count like any
other (two words where C++ makes it one); the subspan, first and last that take their counts as
template arguments are not provided.

# Array<T, N>

std::array: N elements of T in place, an aggregate (`Array<int, 3> a = {1, 2, 3}`, the rest
value-initialized as a C array's; `Array a = {1, 2, 3}` deduces both). `size()`, `max_size()`, `empty()`;
`[i]`, `at(i)` (OutOfRange outside [0, N)), `front()`, `back()`, `data()`; `begin()`, `end()` (pointers),
`cbegin()`, `cend()`, `rbegin()`, `rend()`, `crbegin()`, `crend()`; `fill(value)`, `swap(other)`, `swap(a,
b)`; `==`, `<=>` and what they give, by the elements in order. Tuple-like (tuple.md). `Array<T, 0>` has no
storage: `begin() == end()`, `at` always throws, `[]`, `front()` and `back()` must not be called.
`toArray(cArray)`, `toArray({a, b, c})` (std::to_array): an Array of the elements, copied, or moved from an
rvalue array.
