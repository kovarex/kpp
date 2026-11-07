# Views: Subrange, iotaView, transformView, reverseView

A view is a range computed when it is read, from what it views (what std::views and
std::ranges::subrange convert to). It has `begin()`, `end()`, `empty()` and `size()`, so a
range-for, the algorithms and other views take it like a container. A view is cheap to copy: it
holds iterators, numbers, a function, or a pointer to the container it views. A view of a container
refers to the container, which must outlive it; a view of another view holds a copy of that view.
A view's iterators are of the category of the iterators it is made from (an iota view's are random
access; a reverse view's bidirectional).

- `Subrange<It>(first, last)`: the iterators [first, last) as a range (std::ranges::subrange).
- `iotaView(first, last)`: the numbers first, first + 1, ... up to last, last excluded
  (std::views::iota(first, last)); last is converted to first's type. Its iterator's `*` gives the
  number as a value, not a reference.
- `transformView(range, f)`: f(x) for every element x of range, called each time an element is
  read (std::views::transform(range, f)). `range | transformView(f)` is the same. `front()` and
  `back()` (std's view_interface) are f of the first and the last element (the range not empty;
  back() of a bidirectional one).
- `reverseView(range)`: the elements of a bidirectional range from the last to the first
  (std::views::reverse(range)).

*Differences from C++*: only these four views and the `|` form of transformView; no sentinels, no
`std::ranges` algorithms. An element of a transformView is f's result with the reference f returns
(`&territories.front()` of a view returning `Territory&` is the territory's address).
