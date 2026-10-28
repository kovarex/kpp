# Standard library conformance

The converter turns std::vector into Vector, std::map into Map and so on (compiler/src/convert.cpp,
`stdNames`). Converted code relies on what the C++ standard guarantees about those types, so the
rule (kovarex, 2026-10-28) is: each Kpp container keeps the guarantees of the std type it replaces
(iterator, pointer and reference invalidation, complexity, the meaning of return values), and
anything that behaves differently is either fixed or a separate, explicitly named type. This file
lists, per type, where Kpp stands against the standard. "Differs" items are open unless marked
done; API that is merely missing (a converted call then fails to compile, loudly) is listed last.

Applies to all containers: sizes and indices are `int`, not `size_t` (design.md: Kpp's choice,
containers above 2^31 elements are out of scope), and `at()` does not check the index (std throws
std::out_of_range).

## Map, Set, MultiMap, MultiSet (std::map, set, multimap, multiset)

Conforms (stdlib/core/Map.kpp, 2026-10-28): node-based red-black tree as in all three C++
libraries. Inserting never invalidates iterators, pointers or references; erasing invalidates only
those to the erased entries. Lookup, insert and erase O(log n); iteration in key order; equal keys
of the multi containers stay in insertion order. `insert(entry)` returns Pair<iterator, bool> and
keeps an existing value, `erase(key)` returns the number removed, `erase(iterator)` returns the
next position; `emplace`, `try_emplace`, `equal_range`, `lower_bound`, `upper_bound` as in std.

Differs:
- Only `operator<` orders the keys: no comparator template parameter (std::map<K, V, Compare>).
- `const_iterator` is the same type as `iterator`.
- Kpp additions (not in std): `contains` (C++20 has it too), `insert(key, value)` returning bool,
  camel-case `lowerBound`/`upperBound`.
- Compiler gap: `map.insert({key, makeUnique<T>()})` picks insert(const Pair&) and fails to copy
  the move-only value (spec language/brace_argument_binds_rvalue, pending).

Missing API: rbegin/rend, insert with a hint, emplace_hint, insert of a range, extract, merge,
key_comp, swap.

**FastMap / FastSet** (FastMap.kpp) are the explicit alternative: a B-tree with entries inside
the nodes, faster (docs/stdlib-performance.md), but every insert and erase invalidates all
iterators, pointers and references, as for Vector. Never produced by the converter.

## HashMap, HashSet (std::unordered_map, unordered_set)

Differs (open, needs a decision):
- **References**: std's unordered containers allocate a node per element, so pointers and
  references to elements survive every insert and rehash (only iterators are invalidated by a
  rehash). Kpp's are open addressing in one flat array: any insert that grows the table moves all
  elements, invalidating pointers and references too. Converted code that keeps `&map[key]`
  across inserts breaks.
- **HashMap::insert(key, value) overwrites** an existing value; std::unordered_map::insert keeps
  it and returns Pair<iterator, bool>.
- **HashMap::find returns V\*** (nullptr when absent), not an iterator; HashMap has no
  begin()/end() (only forEach), so range-for over a converted unordered_map fails to compile.
- `erase(key)` returns bool, not a count.
- Hashing is the `hashOf` overload set, not a std::hash-style template parameter.

Missing API: emplace, try_emplace, at, count, iteration on HashMap, bucket and load-factor
functions, reserve.

The way to conform, as for Map: make HashMap/HashSet node-stable with std's API (Abseil's
node_hash_map does this with a flat table of pointers to nodes, keeping most of the speed), and
keep today's table as an explicitly named FlatHashMap/FlatHashSet.

## Vector (std::vector)

Conforms: invalidation (growth invalidates everything; insert and erase invalidate from the
position on), amortized O(1) push_back, exact reserve, elements moved with their move constructors
(2026-10-28), contiguous storage, `push_back(v[0])` safe.

Differs:
- Vector<bool> is a plain Vector of bools, not std's bit-packed specialization with proxy
  references (converted code gets real `bool&`; only memory use differs).
- Kpp additions: `insert(int index, ...)`, `erase(int index)`, `contains`.

Missing API: const overloads of operator[], front and back; assign, shrink_to_fit, swap, insert
of n copies.

## String (std::string)

Conforms: contiguous, always zero-terminated, `data()`/`c_str()` valid for an empty string,
small-string optimization (22 characters in place; std leaves the size unspecified), invalidation
(any non-const operation may invalidate, as in std).

Differs:
- `find` and friends return `int`, with `npos == -1`. Assigned to a `size_t` or compared with
  `npos` this behaves as in C++, but compared with a size it does not: `s.find(x) < s.size()` is
  false in C++ when x is absent (npos is the largest size_t) and true in Kpp (-1 < size).
- `substr(start)` with start past the end returns an empty string; `erase(index)` past the end
  does nothing (std throws std::out_of_range for both).
- Kpp additions: `startsWith`, `endsWith`, `insert(int index, ...)` overloads.

Missing API: compare, rfind of a string, find_last_of, replace with iterators, assign forms.

## Deque, Queue, Stack (std::deque, queue, stack)

Differs (open):
- **Deque** is a Vector with a moving start index: push_front is O(n) once the space before the
  start is used up (std: O(1)); push_back invalidates references when it grows, and pop_front
  occasionally slides all elements down (std::deque keeps references valid on push and pop at
  either end, invalidating iterators only). No insert or erase in the middle.
- **Queue** is a Deque, so it shares this; **Stack** is a Vector (std::stack defaults to a deque:
  references to the elements survive pushes there, not here).

To conform: a real deque (a map of fixed-size blocks, as all three C++ libraries do).

## List, ForwardList, PriorityQueue

- **List** (std::list): the converter maps it, but the type does not exist yet.
- **ForwardList**: node-based like std::forward_list (conforms in behaviour); API is a subset.
- **PriorityQueue**: a max-heap on a Vector, as std::priority_queue on std::vector (conforms).

## Algorithms

Conform: `sort` (introsort, O(n log n), not stable), `stableSort` (stable, O(n log n)),
`lowerBound`, `upperBound`.
Differs: `nthElement` sorts the whole range (O(n log n); std: O(n) on average); the result
satisfies std's postcondition.

## Not audited yet

Optional, UniquePtr, SharedPtr/WeakPtr, Function, Variant (by decision a typed union), Tuple,
Span, Array, StringView, the stream classes and the C library wrappers.
