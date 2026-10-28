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

`less` is a callable taking two elements and returning bool, a strict weak ordering as in
common.md; without it, `operator<` (`Less<>`). `same` is a callable telling whether two elements
are equal; without it, `operator==`. Results that are counts or distances are `ptrdiff_t`.

## Iterator steps

- `distance(first, last)`: the number of steps; O(1) for random access iterators, O(n) otherwise.
- `advance(it, n)`: moves `it` n steps, back for a negative n (bidirectional); O(1) for random
  access iterators, O(|n|) otherwise. `next(it, n = 1)`, `prev(it, n = 1)`: a moved copy.
- `iterSwap(a, b)`: `swap(*a, *b)`.

## Scans (input iterators)

- `find(first, last, value)`, `findIf(first, last, predicate)`, `findIfNot`: the first position
  whose element equals value (satisfies predicate, does not), or last.
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
