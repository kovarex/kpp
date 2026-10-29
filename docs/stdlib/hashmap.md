# HashMap<K, V, Hasher, KeyEqual>, HashSet<T, Hasher, KeyEqual>

Unordered associative containers with unique keys, found through the hasher and the key equality,
`Hash<K>` (`hashOf(key)`) and `EqualTo<K>` (`==`) unless given (see common.md). **HashMap** holds
entries `Pair<K, V>`, **HashSet** values. Iteration visits every entry once, in an unspecified
order that may change with any insertion.

Every entry lives at a fixed address from insertion until it is erased: inserting other entries,
even when the table grows, does not move it.

**FastHashMap<K, V> and FastHashSet<T>** have the same interface and differ in one rule: entries
are stored in the table itself, so whenever the table grows, all pointers and references to
entries are invalidated too (iterators already are). They are faster and smaller, and never
produced by the converter.

## Invalidation

| operation | iterators | pointers and references to entries |
|---|---|---|
| insert, emplace, try_emplace, operator[] that grows the table | all | none |
| the same without growing | none | none |
| erase | the erased entries' | the erased entries' |
| clear, assignment | all | all |
| reserve(n) that grows the table | all | none |

The table grows when an insertion would make `size()` exceed the table's limit for its current
number of buckets; `reserve(n)` makes room for n entries so that inserting up to n entries does
not grow it.

## Complexity

find, contains, count, insert, erase by key, operator[]: O(1) on average with a hash that spreads
the keys, O(n) in the worst case. Iterating over all entries: O(number of buckets + n). Growing:
O(n), amortized O(1) per insert.

## Members (HashMap; HashSet is the same with values for entries, without operator[] and at)

- `HashMap()`; `HashMap(n)`, `HashMap(n, hasher)`, `HashMap(n, hasher, keyEqual)`: empty, with
  room for n entries before the table grows, using copies of the hasher and key equality given;
  `HashMap({{k1, v1}, ...})`; `HashMap(first, last)` (the entries of a range, as insert(first,
  last)); copy and move (the hasher and key equality are copied too).
- `hash_function()`, `key_eq()`: copies of the hasher and the key equality. Types `key_type`,
  `mapped_type` (HashMap), `value_type`, `hasher`, `key_equal`.
- `size()`, `empty()`, `begin()`, `end()` (forward iterators; `*it` is the entry; its key must not
  be changed).
- `find(key)`: an iterator to the entry, or `end()`. `contains(key)`, `count(key)` (0 or 1).
- `operator[](key)`: the value for key, inserting `Pair(key, V())` first when absent (the value
  value-initialized in its entry).
- `at(key)`: the value; throws OutOfRange when absent.
- `insert(entry)`: inserts when the key is absent; an existing entry is left alone, including its
  value. Returns `Pair<iterator, bool>` (the entry with the key, whether it was inserted).
- `emplace(args...)`, `try_emplace(key, args...)`: as for Map. As there, operator[], emplace and
  try_emplace construct the value in its entry and need no copy or move constructor of V
  (FastHashMap moves its entries when it grows, so there V needs a move constructor all the same).
- `insert_or_assign(key, value)`: inserts, or assigns the value of the existing entry.
- `==`: the same keys, each with an equal value, in any order (common.md). There is no ordering.
- `erase(key)`: returns the number removed (0 or 1); `erase(position)`: returns the next position.
- `insert(hint, entry)`, `emplace_hint(hint, args...)`: as insert and emplace, returning the
  entry's position (the hint is accepted and not needed).
- `insert(first, last)`: inserts copies of the entries of a range, one by one.
- `merge(other)`: moves into this map the entries of other whose keys are not present here; in a
  HashMap/HashSet they move node and all (pointers and references stay valid and now refer into
  this map), in the fast tables they are moved. The rest stay in other.
- `swap(other)`: exchanges the contents in O(1).
- `clear()`, `reserve(n)`.
- Kpp additions: `insert(key, value)` (inserts when absent, returns whether it did; an existing
  value is kept), `forEach(f)` calling f(key, value) for every entry.

The hash a key's slot and tag come from is the hasher's result, mixed once more unless the hasher
is the library's `Hash<K>` (whose `hashOf` overloads mix already), so a hasher written for C++ that
returns a key's bits nearly unchanged spreads the keys as well as `hashOf` does.

*Differences from C++*: there is no bucket interface (bucket_count, load_factor, rehash), no
heterogeneous lookup through a transparent hasher, and no allocator parameter; HashMap's fifth
template parameter is `bool stable` (false is FastHashMap), where C++ has the allocator.
