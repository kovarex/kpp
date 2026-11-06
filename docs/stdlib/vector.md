# Vector<T>

A sequence of elements in one contiguous block of memory, in order. Elements are accessed by
index in O(1); adding and removing at the end is amortized O(1); in the middle O(n).

`size()` is the number of elements, `capacity()` the number that fit in the current block
(`capacity() >= size()` always). The block holds `capacity()` slots; the first `size()` hold
elements.

## Invalidation

| operation | iterators, pointers and references |
|---|---|
| any operation that makes `size()` exceed `capacity()` (push_back, emplace_back, insert, resize, reserve) | all invalidated |
| push_back / emplace_back without growing | none (end is) |
| insert at position i without growing | those at i and after, and end |
| erase at position i | those at i and after, and end |
| pop_back | the last element's, and end |
| resize to a smaller size, truncation | the removed elements', and end |
| clear | all elements', and end; the capacity stays |
| reserve(n) with n <= capacity() | none |

Iterators are pointers (`T*`), so "iterator" and "pointer" are invalidated together.

## Members

Construction and assignment:
- `Vector()`: empty, capacity 0, no allocation.
- `Vector(ContainerSize count)`: `count` default-constructed elements. `Vector(count, value)`: count copies
  of value. `Vector(first, last)`: copies of the elements of [first, last), input iterators of
  any kind (pointers, another container's iterators), as `insert(end(), first, last)`; integer
  arguments are a count and a value, never a range.
- `Vector({a, b, c})`: the listed elements, in order. `= {a, b, c}`: replaces the contents with
  them, as `assign({...})`.
- Copy and move construction and assignment: see common.md. Copy assignment reuses nothing it does
  not need: afterwards the vector holds copies of the source's elements.

Access (precondition: the index is in [0, size()); `front`/`back` need a non-empty vector):
- `operator[](i)`, `front()`, `back()`, `data()` (the block; may be null when capacity is 0).
- `at(i)`: like `operator[]`, but throws OutOfRange when i is not in [0, size()).
- `begin()`, `end()`: pointers to the first element and one past the last. Reverse iteration with
  `rbegin()`, `rend()`.

Size and capacity:
- `size()`, `empty()`, `capacity()`.
- A new block receives the elements as std::vector's reallocation does. For an append
  (push_back, emplace_back, an insert at the end), `reserve` and `shrink_to_fit` (the strong
  exception guarantee) it is by move_if_noexcept: an element is moved when its move constructor
  cannot throw or it cannot be copied, and copied otherwise. For an insert before the end it is
  by move, as MSVC's std::vector does (the standard guarantees nothing there).
- `reserve(n)`: afterwards `capacity() >= n`. When n > capacity() the block is replaced by one
  with capacity exactly n and the elements are relocated; otherwise nothing happens. O(size()).
- `resize(n)`, `resize(n, value)`: removes elements from the end, or appends default-constructed
  elements (or copies of `value`; `value` may be an element of this vector) until `size() == n`.
- `shrink_to_fit()`: reduces the capacity to `size()`, relocating the elements (invalidates all).
- `assign(count, value)`, `assign(first, last)` (input iterators of any kind), `assign({...})`:
  replace the contents (value may be an element; the range must not be).

Adding and removing:
- `push_back(value)`, `emplace_back(args...)`: append one element (copied, moved, or built from
  the arguments). When the vector is full the capacity grows geometrically (to at least twice the
  old one), so n appends cost O(n) in total. The argument may refer to an element of this vector:
  the new element is built before the old ones are relocated.
- `pop_back()`: removes the last element. Precondition: not empty.
- `insert(position, value)`: inserts before position (a pointer into the vector, const or not, or
  `end()`); returns a pointer to the new element. The elements from position on are relocated one
  place up. O(size() - position + 1). `value` may be an element of this vector.
- `insert(position, first, last)`: inserts the elements of [first, last), input iterators of any
  kind: copies, or moves through a MoveIterator. A range of this vector's own elements is copied
  out first and inserted as it was (*difference from C++*, where it is a precondition that the
  range is another's; MSVC's and libstdc++'s vectors take it too). Returns a
  pointer to the first inserted one. For forward iterators the room is opened once
  (O(size() - position + n)); a single-pass range goes in one element at a time.
  `insert(position, {a, b})`: the list's elements. `insert(position, count, value)`: count copies
  of value (which may be an element).
- `emplace(position, args...)`: like insert, with an element built from the arguments: at the end in
  its place, as emplace_back builds it; before another element through a temporary that is moved
  in (the arguments may be elements the insertion moves).
- `erase(position)`, `erase(first, last)`: move-assigns the following elements down and destroys the
  last ones, one destructor call per erased element, as std::vector does; returns a pointer to the
  element after the removed ones (now at the removed position). A T without move assignment (which
  std::vector cannot erase) is relocated down instead.
- `clear()`: destroys all elements; the capacity stays.
- `swap(other)`: exchanges the contents, O(1).

Comparison: `==` and `<=>` (common.md): equal with equal elements in order, ordered
lexicographically.

kpp additions (not in C++): `insert(index, value)`, `erase(index)` taking an index of any integer
type (so that a literal 0 is the index, not a null position),
`contains(value)` (linear search with `==`). A Vector converts to `Span<T>` of its elements.

*Difference from C++*: there is no packed `Vector<bool>`; a Vector of bools stores one bool per
element and gives real `bool&` references.
