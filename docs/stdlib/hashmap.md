# HashMap<K, V>, HashSet<T>

Unordered associative containers with unique keys, found through `hashOf(key)` and `==` (see
common.md). **HashMap** holds entries `Pair<K, V>`, **HashSet** values. Iteration visits every
entry once, in an unspecified order that may change with any insertion.

Every entry lives at a fixed address from insertion until it is erased: inserting other entries,
even when the table grows, does not move it. (A faster table that moves entries would be a
separate, explicitly named type, as FastMap is for Map.)

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

- `HashMap()`, copy and move.
- `size()`, `empty()`, `begin()`, `end()` (forward iterators; `*it` is the entry; its key must not
  be changed).
- `find(key)`: an iterator to the entry, or `end()`. `contains(key)`, `count(key)` (0 or 1).
- `operator[](key)`: the value for key, inserting `Pair(key, V())` first when absent.
- `at(key)`: the value; throws OutOfRange when absent.
- `insert(entry)`: inserts when the key is absent; an existing entry is left alone, including its
  value. Returns `Pair<iterator, bool>` (the entry with the key, whether it was inserted).
- `emplace(args...)`, `try_emplace(key, args...)`: as for Map.
- `insert_or_assign(key, value)`: inserts, or assigns the value of the existing entry.
- `erase(key)`: returns the number removed (0 or 1); `erase(position)`: returns the next position.
- `clear()`, `reserve(n)`.

*Differences from C++*: hashing is the `hashOf` overload set rather than a hash type parameter;
there is no bucket interface (bucket_count, load_factor, rehash).
