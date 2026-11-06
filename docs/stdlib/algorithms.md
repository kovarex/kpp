# Algorithms

Functions over ranges [first, last) given by two iterators (what std's `<algorithm>`,
`<numeric>` and `<iterator>` functions convert to). Each one takes any iterator of the category
it needs:

- **input** iterators (`*it`, `++it`, `==`): the scans that read the range once;
- **forward** iterators (an input iterator that can be copied and read again, and written through):
  the algorithms that rearrange in place, and the binary searches;
- **bidirectional** iterators (also `--it`): reverse, reverseCopy, nextPermutation, prevPermutation,
  copyBackward;
- **random access** iterators (also `it + n`, `it - n`, `a - b`, `a < b`, `it[n]`): sorting, partial
  sorts and heaps.

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
  `forEachN(first, n, f)`: for the first n elements (none for n <= 0); returns the position after them.
- `minElement(first, last [, less])`, `maxElement(first, last [, less])`: the first least
  (greatest) element, or last for an empty range. `minmaxElement(first, last [, less])`: the pair
  (the first least, the last greatest), both last for an empty range; at most 3/2 (n - 1) comparisons.
- `equal(first1, last1, first2 [, same])`: whether the range equals the one from first2 of the same
  length; `equal(first1, last1, first2, last2)`: whether the two ranges are equal, lengths included.
- `lexicographicalCompare(first1, last1, first2, last2 [, less])`: whether the first range comes
  before the second in dictionary order.
- `mismatch(first1, last1, first2 [, same])`, `mismatch(first1, last1, first2, last2 [, same])`: the
  pair of first positions where the ranges differ, or where the first range (either range) ends.
- `adjacentFind(first, last [, same])`: the first of two equal neighbours, or last.
- `findFirstOf(first1, last1, first2, last2 [, same])`: the first element of the first range equal to
  any of the second, or last1.
- `accumulate(first, last, init [, op])`: init with every element added in order
  (`init = init + x`, or `init = op(init, x)`).
- `min({a, b, c})`, `max({a, b, c})`: of a brace list.
- `minmax(a, b [, less])`: a Pair of references to the arguments, (b, a) when b is less than a and
  (a, b) otherwise. `minmax({a, b, c} [, less])`: a Pair of copies of the first least and the last
  greatest; the list must not be empty.

## Searching for subranges (forward iterators)

- `search(first1, last1, first2, last2 [, same])`: the first place the second range occurs in the
  first, first1 for an empty second range, last1 when it does not occur.
- `findEnd(first1, last1, first2, last2 [, same])`: the last such place, last1 for an empty second
  range or when it does not occur.
- `searchN(first, last, n, value [, same])`: the first run of n elements equal to value (`same(x,
  value)`), first for n <= 0, last when there is none.
- `isPermutation(first1, last1, first2 [, same])`, `isPermutation(first1, last1, first2, last2 [,
  same])`: whether the second range holds the same elements as the first, as many of each, in any
  order. O(n^2) comparisons.

## Numeric

std's `<numeric>` under kpp's names, each as std's, run in order:
- `reduce(first, last [, init [, op]])` (init a value-initialized element when omitted),
  `innerProduct(first1, last1, first2, init [, sum, product])`, `transformReduce(first1, last1,
  first2, init [, reduce, transform])` and `transformReduce(first, last, init, reduce, transform)`.
- `partialSum(first, last, out [, op])`, `adjacentDifference(first, last, out [, op])`,
  `inclusiveScan(first, last, out [, op [, init]])`, `exclusiveScan(first, last, out, init [, op])`,
  `transformInclusiveScan(first, last, out, op, transform [, init])`, `transformExclusiveScan(first,
  last, out, init, op, transform)`: out may be first.
- `iota(first, last, value)`: value, value + 1, ... over the range.
- `gcd(m, n)`, `lcm(m, n)`: of any two integer types, in their common type, on the absolute values.
- `midpoint(a, b)`: of integers without overflow, rounded towards a; of floating point values without
  overflow; of two pointers into one array. `lerp(a, b, t)`: exact at t 0 and 1, monotonic, a for a == b.

## Bits

C++20's `<bit>` under kpp's names, each as std's, for the unsigned integer types (no signed type,
bool or char, as in C++), all folding at compile time: `rotl(x, count)`, `rotr(x, count)` (count
modulo the width, a negative one rotating the other way), `popcount(x)`, `countlZero(x)`,
`countlOne(x)`, `countrZero(x)`, `countrOne(x)`, `bitWidth(x)`, `bitCeil(x)`, `bitFloor(x)`,
`hasSingleBit(x)`; `byteswap(x)` of any integer type; `bitCast<To>(from)` between types of one size.

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
  `setIntersection(...)`: those in both. `setUnion(...)`: those in either, an element as many times
  as the range with more of it has it, the first range's before the second's. `setSymmetricDifference(...)`:
  those in one range and not the other, as many times as the one range has more, in order.
- `includes(first1, last1, first2, last2 [, less])`: whether every element of the second sorted range
  is in the first, as many times.
- `removeCopy(first, last, out, value)`, `removeCopyIf(first, last, out, predicate)`: the elements
  that are not equal to value (do not satisfy predicate), in order.
- `replaceCopy(first, last, out, old, new)`, `replaceCopyIf(first, last, out, predicate, new)`: the
  range with new written for each element equal to old (satisfying predicate).
- `uniqueCopy(first, last, out [, same])`: the first of every run of equal neighbours.
- `reverseCopy(first, last, out)` (bidirectional): the range backwards. `rotateCopy(first, middle,
  last, out)`: [middle, last), then [first, middle).
- `partitionCopy(first, last, outTrue, outFalse, predicate)`: the elements that satisfy predicate to
  outTrue, the others to outFalse, in order; returns the Pair of both ends.

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
- `swapRanges(first1, last1, first2)`: exchanges the range with the one of its length from first2;
  returns the end of that one. They must not overlap.
- `reverse(first, last)` (bidirectional).
- `rotate(first, middle, last)`: moves middle's element to first, keeping the cyclic order;
  returns the new position of first's element. O(n).
- `nextPermutation(first, last [, less])` (bidirectional): the next greater arrangement in
  dictionary order, returning true; from the greatest, the least one (sorted), returning false.
  `prevPermutation(first, last [, less])`: the next smaller one; from the least, the greatest one,
  returning false.
- `partition(first, last, predicate)`: moves the elements that satisfy predicate before the others,
  by swaps, and returns the first of the others. Not stable; exactly n predicate calls.
- `stablePartition(first, last, predicate)`: the same, each group keeping its order; moves the elements
  from the first that fails on through a temporary buffer of them; exactly n predicate calls.
- `isPartitioned(first, last, predicate)`: whether no element that satisfies predicate comes after
  one that does not; true for an empty range.
- `partitionPoint(first, last, predicate)`: the end of the satisfying elements of a partitioned range,
  O(log n) predicate calls.

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
- `partialSort(first, middle, last [, less])`: the least middle - first elements, sorted, in
  [first, middle); the others after them in no particular order. O(n log m) comparisons, m the sorted part.
- `partialSortCopy(first, last, outFirst, outLast [, less])`: the least elements of the range, sorted,
  written to [outFirst, outLast) (random access) as far as both reach; returns the end of what was
  written. The range is not changed, and can be of input iterators.
- `inplaceMerge(first, middle, last [, less])`: the sorted ranges [first, middle) and [middle, last)
  become one sorted range, stable, the first range's element first on ties; through a temporary buffer
  of the first range, with n - 1 comparisons at most. Forward iterators are enough.
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

## Heaps (random access iterators)

A max-heap under less: no element is less than the ones below it (the children of position i are at
2i + 1 and 2i + 2), so its first element is a greatest one. How equal elements end up arranged is
unspecified.

- `makeHeap(first, last [, less])`: arranges the range as a heap. O(n) comparisons.
- `pushHeap(first, last [, less])`: adds the element at last - 1 to the heap [first, last - 1). O(log n).
- `popHeap(first, last [, less])`: moves the first element to last - 1, and [first, last - 1) is a heap
  again. O(log n).
- `sortHeap(first, last [, less])`: a heap sorted ascending. Not stable. O(n log n).
- `isHeap(first, last [, less])`: whether the range is a heap; `isHeapUntil(first, last [, less])`: the
  end of the longest prefix that is one.

## Not specified yet

shuffle, sample, shiftLeft and shiftRight; clamp with a less, min and max of a brace list with a less.
None of them has a user in the converted code yet.
