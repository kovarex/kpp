# Algorithms

Functions over ranges [first, last) of pointers (and of iterators where a function says so).
`less` is a callable taking two elements and returning bool, a strict weak ordering as in
common.md; without it, `operator<`.

## Sorting

- `sort(first, last)`, `sort(first, last, less)`: arranges the range in ascending order. Not
  stable: the order of equivalent elements afterwards is unspecified. O(n log n) comparisons and
  moves in the worst case. Elements are exchanged with `swap` and moved with move construction and
  move assignment.
- `stableSort(first, last)`, `stableSort(first, last, less)`: ascending, equivalent elements keep
  their relative order. O(n log n) comparisons; uses a temporary buffer of n elements (relocating
  elements into it and back).
- `nthElement(first, nth, last)`, `nthElement(first, nth, last, less)`: afterwards *nth is the
  element that would be there if the range were sorted, nothing before it is greater and nothing
  after it is less. O(n) on average.
- `isSorted(first, last)`, `isSorted(first, last, less)`: whether no element is less than the one
  before it. O(n).

## Searching sorted ranges

The range must be partitioned by the value (sorted ascending is enough). O(log n) comparisons.
- `lowerBound(first, last, value)`, with an optional less: the first position whose element is not
  less than value, or last.
- `upperBound(first, last, value)`, with an optional less: the first position whose element is
  greater than value, or last.

## Not specified yet

The library also has `find`, `findIf`, `count`, `countIf`, `allOf`, `anyOf`, `noneOf`, `remove`,
`removeIf`, `reverse`, `fill`, `copy`, `transform`, `accumulate`, `minElement`, `maxElement`,
`unique`, `rotate`, `min`, `max`, `clamp`, `swap` and `exchange`; their pages are still to be
written.
