# FastMap<K, V>, FastSet<T>

Ordered associative containers with the interface of Map and Set (map.md) and one difference:
entries are stored many to a block and move between blocks as others are inserted and erased.
That makes them faster and smaller than Map and Set (fewer allocations, lookups touch fewer cache
lines), and it is why they are separate types: code that keeps iterators, pointers or references
across modifications must use Map or Set. The converter never produces them; choosing one is a
decision made in Kpp code.

## Invalidation

| operation | iterators, pointers and references |
|---|---|
| insert, emplace, try_emplace, operator[] that adds an entry | all |
| erase | all |
| operator[], find and the other lookups that add nothing | none |
| clear, assignment | all |

## Complexity

As Map: O(log n) lookups, inserts and erases; O(n) iteration over all entries. `erase(position)`
costs O(log n) (it erases by key and looks the next entry up again).

## Members

Those of Map and Set without the multi variants: construction, size, empty, begin, end, find,
contains, count, lowerBound/lower_bound, upperBound/upper_bound, equal_range, operator[] and at
(FastMap), insert (returning `Pair<iterator, bool>`), emplace, try_emplace (FastMap), insert(key,
value) (FastMap), erase(key) (returning the number removed), erase(position) (returning the next
position), clear. `erase(key)` may be given a key that refers to an entry of this container.
