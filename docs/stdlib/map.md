# Map<K, V>, Set<T>, MultiMap<K, V>, MultiSet<T>

Ordered associative containers. **Map** holds entries `Pair<K, V>` (`first` is the key, `second`
the value) with unique keys, **Set** unique values; **MultiMap** and **MultiSet** allow
equivalent keys (see common.md for equivalence and the ordering requirement). Iteration visits the
entries in ascending key order; equivalent keys of the multi containers in the order they were
inserted.

Every entry lives at a fixed address from insertion until it is erased: other inserts and erases
never move it. (FastMap and FastSet, fastmap.md, give that up for speed.)

## Invalidation

| operation | iterators, pointers and references |
|---|---|
| insert, emplace, try_emplace, operator[] | none |
| erase | only those to the erased entries |
| clear | all except end |
| copy or move assignment to the container | all except end |

## Complexity

Lookups (find, contains, count, lowerBound, upperBound, equal_range), single inserts and erases
by key: O(log n) comparisons. Erasing at an iterator: amortized O(1) plus rebalancing, O(log n)
worst case. `++it` and `--it`: amortized O(1), O(log n) worst case. Iterating over all entries:
O(n). `count` in a multi container: O(log n + number found).

## Members (Map; Set is the same with values for entries)

- `Map()`, `Map({{k1, v1}, {k2, v2}})` (later duplicates of a key are ignored), copy and move.
- `size()`, `empty()`, `begin()`, `end()`. Iterators are bidirectional; `*it` is the entry. The
  key of an entry must not be changed through an iterator (the order would break); the value may.
- `find(key)`: an iterator to the entry with an equivalent key, or `end()`. In a multi container,
  the first of them.
- `contains(key)`, `count(key)` (0 or 1; the number of equivalent keys in a multi container).
- `lowerBound(key)` / `lower_bound`: the first entry whose key is not less than key.
  `upperBound(key)` / `upper_bound`: the first whose key is greater. `equal_range(key)`: the pair
  (lower bound, upper bound). Each returns `end()` when there is no such entry.
- `operator[](key)`: a reference to the value of key's entry, inserting `Pair(key, V())` first when
  there is none. Map only.
- `at(key)`: the value of key's entry; throws OutOfRange when there is none. Map only.
- `insert(entry)`: inserts when no equivalent key is present (an existing entry is left alone,
  including its value). Returns `Pair<iterator, bool>`: the entry with the key and whether it was
  inserted. In a multi container it always inserts (after the equivalent keys) and returns the
  iterator. The entry may be passed as an rvalue to be moved in.
- `emplace(args...)`: builds an entry from the arguments, then as insert.
- `try_emplace(key, args...)`: when key is absent, inserts `Pair(key, V(args...))`; when present,
  does not touch the arguments. Returns `Pair<iterator, bool>`. Map only.
- `erase(key)`: removes the entry (in a multi container all entries) with an equivalent key;
  returns how many were removed. `key` may refer to an entry of this container.
- `erase(position)`: removes that entry, returns an iterator to the next one. `erase(first,
  last)`: removes the range, returns last.
- `clear()`.

Kpp additions (not in C++): `insert(key, value)` returning whether the key was new (Map), and the
camel-case spellings `lowerBound`, `upperBound`.

*Differences from C++*: only `operator<` orders the keys (no comparator parameter); there is no
separate const_iterator type.
