# Map<K, V, Compare>, Set<T, Compare>, MultiMap<K, V, Compare>, MultiSet<T, Compare>

Ordered associative containers. **Map** holds entries `Pair<K, V>` (`first` is the key, `second`
the value) with unique keys, **Set** unique values; **MultiMap** and **MultiSet** allow
equivalent keys. The keys are ordered by the comparator Compare, `Less<K>` (`operator<`) unless
given (see common.md for comparators, equivalence and the ordering requirement). Iteration visits
the entries in ascending key order under the comparator; equivalent keys of the multi containers
in the order they were inserted.

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

- `Map()`, `Map(compare)` (an empty map ordered by a copy of compare), `Map({{k1, v1}, {k2,
  v2}})` and `Map({...}, compare)` (later duplicates of a key are ignored), `Map(first, last)` and
  `Map(first, last, compare)` (the entries of a range, as insert(first, last)), copy and move (the
  comparator is copied too). `= {...}` replaces the entries with the listed ones, as the list
  constructor builds them (the comparator stays).
- `key_comp()`: a copy of the comparator. Types `key_type`, `mapped_type` (maps), `value_type`,
  `key_compare`.
- `size()`, `empty()`, `begin()`, `end()`. Iterators are bidirectional; `*it` is the entry. The
  key of an entry must not be changed through an iterator (the order would break); the value may.
- `find(key)`: an iterator to the entry with an equivalent key, or `end()`. In a multi container,
  the first of them.
- `contains(key)`, `count(key)` (0 or 1; the number of equivalent keys in a multi container).
- `lowerBound(key)` / `lower_bound`: the first entry whose key is not less than key.
  `upperBound(key)` / `upper_bound`: the first whose key is greater. `equal_range(key)`: the pair
  (lower bound, upper bound). Each returns `end()` when there is no such entry.
- The lookups above also take a key of another type: compared as it is when the comparator is
  transparent (common.md), converted to the key type once otherwise. With a transparent
  comparator, `count` of a unique container can exceed 1 when several keys are equivalent to the
  argument.
- `operator[](key)`: a reference to the value of key's entry, inserting `Pair(key, V())` first when
  there is none, its value value-initialized in the new node. Map only.
- `at(key)`: the value of key's entry; throws OutOfRange when there is none. Map only.
- `insert(entry)`: inserts when no equivalent key is present (an existing entry is left alone,
  including its value). Returns `Pair<iterator, bool>`: the entry with the key and whether it was
  inserted. In a multi container it always inserts (after the equivalent keys) and returns the
  iterator. The entry may be passed as an rvalue to be moved in.
- `insert({...})`: inserts the listed entries (values of a set) one by one, as insert(entry) does.
- `emplace(args...)`: constructs an entry from the arguments, then as insert (the entry is not
  copied or moved: when its key is present, it is destroyed).
- `try_emplace(key, args...)`: when key is absent, inserts an entry of key and the value
  constructed from the arguments, `V(args...)`, in the new node; when present, does not touch the
  arguments. Returns `Pair<iterator, bool>`. Map only. Like operator[] and emplace, it neither
  copies nor moves the value, so V needs no copy or move constructor for them (`Map<Path, File>`
  with File's move constructor deleted). An rvalue key (`try_emplace(move(key), args...)`) is moved
  into the new entry, a const one copied; a key of another type (a LowercaseString, a StringView)
  is converted to K by the copying overload, never moved from. `try_emplace(hint, key, args...)`: the same, returning
  the entry's position (new or found); the hint is not used, O(log n).
- `erase(key)`: removes the entry (in a multi container all entries) with an equivalent key;
  returns how many were removed. `key` may refer to an entry of this container.
- `erase(position)`: removes that entry, returns an iterator to the next one. `erase(first,
  last)`: removes the range, returns last.
- `rbegin()`, `rend()`: reverse iteration, from the greatest key.
- `insert(hint, entry)`, `emplace_hint(hint, args...)`: as insert and emplace, returning the
  entry's position; hint is a position in this container. When the new entry belongs right before
  hint, the insert costs O(1) amortized instead of O(log n).
- `insert(first, last)`: inserts copies of the entries of a range of any iterator type, as
  insert(entry) one by one.
- `extract(position)`, `extract(key)`: takes the entry out of the container *together with its
  node* and returns it in a node handle (empty when the key is absent): no copy or move of the
  entry; pointers and references to it stay valid while it is in the handle and after it is
  inserted again. The handle (MapNodeHandle for the maps, SetNodeHandle for the sets) is move-only;
  `empty()`, `key()` and `mapped()` (maps) or `value()` (sets); the key may be changed while the
  entry is in the handle. Destroying a non-empty handle destroys the entry.
- `insert(handle)`: puts the handle's node in, as insert(entry). Unique containers return
  NodeInsertReturn: `position` (the entry with the key), `inserted`, and `node` (the handle back
  when the key was present); the multi containers return the position. An empty handle inserts
  nothing.
- `merge(other)`: moves into this container the entries of other (of the same kind and key and
  value types, its comparator may differ) whose keys are not present here (all of them for the
  multi containers), node and all: pointers and references to them stay valid and now refer into
  this container. The rest stay in other.
- `swap(other)`: exchanges the contents and the comparators in O(1); iterators, pointers and
  references keep referring to the same entries, now in the other container (end() excepted).
- `clear()`.
- `==` and `<=>` (common.md): equal with equal entries in order (keys and values by their `==`),
  ordered lexicographically by the entries (a Pair orders by its first, then its second).

kpp additions (not in C++): `insert(key, value)` returning whether the key was new (Map), and the
camel-case spellings `lowerBound`, `upperBound`.

*Differences from C++*: no `value_comp()`, and no allocator parameter.
