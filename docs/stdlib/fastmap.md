# FastMap<K, V>, FastSet<T>

Ordered associative containers with the interface of Map and Set (map.md) and one difference:
entries are stored many to a block and move between blocks as others are inserted and erased.
That makes them faster and smaller than Map and Set (fewer allocations, lookups touch fewer cache
lines), and it is why they are separate types: code that keeps iterators, pointers or references
across modifications must use Map or Set. The converter never produces them; choosing one is a
decision made in kpp code.

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

Those of Map and Set without the multi variants: construction, `= {...}`, size, empty, begin, end, find,
contains, count, lowerBound/lower_bound, upperBound/upper_bound, equal_range, operator[] and at
(FastMap), insert (returning `Pair<iterator, bool>`), emplace, try_emplace and try_emplace(hint, key, args...) (FastMap), insert(key,
value) (FastMap), erase(key) (returning the number removed), erase(position) (returning the next
position), clear, rbegin, rend, insert(hint, entry) and emplace_hint (the hint is not used: O(log n)),
insert(first, last), merge(other) (moving the entries whose keys are absent here; moving, so
it invalidates pointers into both), swap. `erase(key)` may be given a key that refers to an entry
of this container. There are no node handles (extract): entries do not have nodes of their own.

FastMap and FastSet are ordered by `operator<` only: they have no comparator parameter (Map and
Set do), so the constructors and key_comp() that take or give one, and lookups by keys of other
types, are not there.
