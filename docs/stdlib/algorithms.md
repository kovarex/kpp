# Algorithms

Functions over ranges [first, last) given by two iterators (what std's `<algorithm>`,
`<numeric>` and `<iterator>` functions convert to). Each one takes any iterator of the category
it needs:

- **input** iterators (`*it`, `++it`, `==`): the scans that read the range once;
- **forward** iterators (an input iterator that can be copied and read again, and written through):
  the algorithms that rearrange in place, and the binary searches;
- **bidirectional** iterators (also `--it`): reverse, nextPermutation, copyBackward;
- **random access** iterators (also `it + n`, `it - n`, `a - b`, `a < b`, `it[n]`): sorting.

An iterator type says its category in `iterator_category` (`InputIteratorTag`,
`ForwardIteratorTag`, `BidirectionalIteratorTag`, `RandomAccessIteratorTag`, `OutputIteratorTag`;
each tag derives from the one before), read through `IteratorTraits<It>` together with
`value_type`, `difference_type`, `pointer` and `reference`. Pointers are random access iterators
of their element type. The library's iterators: Vector, String, StringView, Array and Span use
pointers; Deque's are random access; Map, Set, MultiMap, MultiSet, List, FastMap and FastSet's
are bidirectional; HashMap, HashSet and ForwardList's are forward; `backInserter(c)` and
`inserter(c, it)` give output iterators.

`ReverseIterator<It>` (std::reverse_iterator) walks a bidirectional iterator It (a pointer, or a
class's iterator) backwards. `ReverseIterator(it)` refers to the element before it, so a range's
`rbegin()` is `ReverseIterator(end())` and `rend()` is `ReverseIterator(begin())`; `base()` gives
it back. `*`, `->`, `++` (one element towards the front), `--`; for a random access It also `+ n`,
`- n`, `+=`, `-=`, `[n]`, the distance `a - b` and the orderings, all reversed (`a < b` when a is
nearer the back). `==`, `!=` and the orderings compare with a ReverseIterator of another It as
well, and a ReverseIterator converts to the one of an iterator its It converts to (a
`const_reverse_iterator` from a `reverse_iterator`). Its member types are It's, through
IteratorTraits; `iterator_type` is It. `makeReverseIterator(it)` (std::make_reverse_iterator)
makes one. The containers' reverse iterators are ReverseIterators of their iterators.

`less` is a callable taking two elements and returning bool, a strict weak ordering as in
common.md; without it, `operator<` (`Less<>`). `same` is a callable telling whether two elements
are equal; without it, `operator==`. Results that are counts or distances are `ptrdiff_t`.

## Iterator steps

- `distance(first, last)`: the number of steps; O(1) for random access iterators, O(n) otherwise.
- `advance(it, n)`: moves `it` n steps, back for a negative n (bidirectional); O(1) for random
  access iterators, O(|n|) otherwise. `next(it, n = 1)`, `prev(it, n = 1)`: a moved copy.
- `iterSwap(a, b)`: `swap(*a, *b)`.
- `toAddress(p)` (std::to_address): the address an iterator or fancy pointer refers to; a pointer
  as it is, a class through `p.operator->()`, taken again until it is a pointer.

## Scans (input iterators)

- `find(first, last, value)`, `findIf(first, last, predicate)`, `findIfNot`: the first position
  whose element equals value (satisfies predicate, does not), or last. `find(range, value)` and
  `findIf(range, predicate)` (std::ranges::find, find_if) do the same over a whole range, a
  container or a C array taken by reference, from `begin(range)` to `end(range)`; without
  std::ranges' projections, and an rvalue range does not bind.
- `count(first, last, value)`, `countIf(first, last, predicate)`.
- `allOf`, `anyOf`, `noneOf(first, last, predicate)`; allOf and noneOf are true for an empty range.
- `forEach(first, last, f)`: calls f(x) for every element in order; returns f.
- `minElement(first, last [, less])`, `maxElement(first, last [, less])`: the first least
  (greatest) element, or last for an empty range.
- `equal(first1, last1, first2 [, same])`: whether the range equals the one from first2 of the same
  length; `equal(first1, last1, first2, last2)`: whether the two ranges are equal, lengths included.
- `lexicographicalCompare(first1, last1, first2, last2 [, less])`: whether the first range comes
  before the second in dictionary order.
- `adjacentFind(first, last [, same])`: the first of two equal neighbours, or last.
- `accumulate(first, last, init [, op])`: init with every element added in order
  (`init = init + x`, or `init = op(init, x)`).
- `min({a, b, c})`, `max({a, b, c})`: of a brace list.

## Writing

The output position `out` is any iterator that can be written through and stepped, an output
iterator included; the functions return the position after the last element written.

- `copy(first, last, out)`, `copyIf(first, last, out, predicate)`, `copyN(first, n, out)`.
- `move(first, last, out)` (three arguments): as copy, moving the elements.
- `copyBackward(first, last, outLast)`, `moveBackward(first, last, outLast)`: copy or move to the
  range that ends at outLast, from the last element back, so that it may overlap [first, last)
  when outLast is after last; return the start of the written range.
- `transform(first, last, out, f)`: writes f(x) for every element; `transform(first1, last1,
  first2, out, f)`: f(x, y) for the elements of two ranges in pairs.
- `fill(first, last, value)`, `fillN(first, n, value)`, `generate(first, last, g)`,
  `generateN(first, n, g)` (g() for every position).
- `replace(first, last, old, new)`, `replaceIf(first, last, predicate, new)`.
- `merge(first1, last1, first2, last2, out [, less])`: two sorted ranges merged; on ties the first
  range's element first.
- `setDifference(first1, last1, first2, last2, out [, less])`: the elements of the first sorted
  range that are not in the second (as many times as the first has more), in order.
  `setIntersection(...)`: those in both.

## Rearranging in place (forward iterators)

- `remove(first, last, value)`, `removeIf(first, last, predicate)`: move the elements to keep to
  the front, in order, and return the end of them; the elements after it are valid but unspecified
  (moved from). Erasing them is the container's job: `v.erase(removeIf(v.begin(), v.end(), p),
  v.end())`.
- `eraseIf(container, predicate)`: erases the elements the predicate holds for and returns how
  many (std::erase_if). A map's or set's (the predicate takes the entry) and a List's one by one,
  the other elements' iterators staying valid; a Vector's, Deque's or String's by removeIf and one
  erase, the others keeping their order. O(n) predicate calls (2026-10-30).
- `unique(first, last [, same])`: keeps the first of every run of equal neighbours, moved to the
  front; returns the end of the kept ones.
- `reverse(first, last)` (bidirectional).
- `rotate(first, middle, last)`: moves middle's element to first, keeping the cyclic order;
  returns the new position of first's element. O(n).
- `nextPermutation(first, last [, less])` (bidirectional): the next greater arrangement in
  dictionary order, returning true; from the greatest, the least one (sorted), returning false.

## Sorting (random access iterators)

- `sort(first, last [, less])`: arranges the range in ascending order. Not stable: the order of
  equivalent elements afterwards is unspecified. O(n log n) comparisons and moves in the worst
  case. Elements are exchanged with `swap` and moved with move construction and move assignment.
- `stableSort(first, last [, less])`: ascending, equivalent elements keep their relative order.
  O(n log n) comparisons; uses a temporary buffer of n elements (relocating elements into it and
  back when the range is an array, moving them otherwise).
- `nthElement(first, nth, last [, less])`: afterwards *nth is the element that would be there if
  the range were sorted, nothing before it is greater and nothing after it is less. O(n) on
  average.
- `isSorted(first, last [, less])`: whether no element is less than the one before it;
  `isSortedUntil(first, last [, less])`: the end of the longest sorted prefix. O(n), forward
  iterators.

## Searching sorted ranges (forward iterators)

The range must be partitioned by the value (sorted ascending under less is enough). O(log n)
comparisons; O(log n) iterator steps for random access iterators, O(n) for the others.

- `lowerBound(first, last, value [, less])`: the first position whose element is not less than
  value, or last.
- `upperBound(first, last, value [, less])`: the first position whose element is greater than
  value, or last.
- `equalRange(first, last, value [, less])`: the pair (lowerBound, upperBound).
- `binarySearch(first, last, value [, less])`: whether an element equivalent to value is there.

## Not specified yet

Partitioning (partition, stablePartition), partial sorts, the heap functions, set union and
symmetric difference, searches for subranges (search, findEnd, mismatch); none of them has a
user in the converted code yet.
