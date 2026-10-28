# Standard library conformance

The specification of the library is docs/stdlib/ (self-contained: it started from the C++
standard library's guarantees, which converted code relies on, and writes them out). This file
lists where the implementation in stdlib/ does not meet that specification yet. Each item is a bug
to fix, or, if the rule should change instead, a decision to record in the specification and in
design.md. Behaviour that differs silently comes first; API that is missing (a call then fails to
compile, loudly) last. State of 2026-10-28.

## Behaviour that differs

**HashMap, HashSet** (hashmap.md):
- Entries move when the table grows: the implementation is open addressing in one flat array, so
  pointers and references to entries are invalidated by any insert that grows the table (the
  specification keeps them valid, as std::unordered_map's node-based tables do).
- `HashMap::insert(key, value)` assigns the value when the key exists; the specified
  `insert(entry)` keeps the existing value and returns `Pair<iterator, bool>`.
- `HashMap::find` returns `V*` (nullptr when absent) instead of an iterator, and HashMap cannot be
  iterated (only `forEach`).
- `erase(key)` returns bool instead of a count.
- Proposed fix: make HashMap/HashSet node-stable with the specified interface (a flat table of
  pointers to nodes keeps most of the speed), and keep today's table as an explicitly named type
  such as FastHashMap, as FastMap is for Map.

**Deque, Queue, Stack** (deque.md):
- Deque is a Vector with a moving start: push_front is O(n) once the room before the start is
  used up, push_back invalidates references when it grows, and pop_front now and then slides all
  elements down. The specification (a deque of fixed-size blocks) keeps references valid for
  pushes and pops at both ends.
- Stack is built on Vector instead of Deque, so pushes invalidate references to elements.

**String** (string.md):
- `substr(start)` and `erase(index)` with start/index past the end return an empty string / do
  nothing instead of throwing OutOfRange.

**All containers**: `at()` does not check its argument (specified: throws OutOfRange).

**Algorithms** (algorithms.md): `nthElement` sorts the whole range, O(n log n) instead of O(n) on
average (the result is right).

**Compiler gaps that break library use** (pending spec snippets in compiler/tests/spec):
- `map.insert({key, makeUnique<T>()})` picks the `const Pair&` overload over `Pair&&` and then
  cannot copy the move-only value (language/brace_argument_binds_rvalue).
- `KeyOf::of(a) < b` with a class type emits invalid IR; Map.kpp and FastMap.kpp avoid the form
  (templates/static_call_through_parameter_operator).

## Missing types and API

- **List**: the converter maps std::list to it, but it does not exist.
- **Map, Set, MultiMap, MultiSet, FastMap, FastSet**: rbegin/rend, insert with a hint,
  emplace_hint, insert of a range, extract, merge, swap.
- **HashMap, HashSet**: begin/end on HashMap, emplace, try_emplace, insert_or_assign, at, count,
  reserve.
- **Vector**: `capacity` is a field, not `capacity()`; const overloads of operator[], front and
  back; shrink_to_fit, swap.
- **String**: rfind of a string, find_last_of.
- **Deque**: insert and erase in the middle, emplace_front/emplace_back, at, resize.
- **ForwardList**: insert_after, erase_after, before_begin.
- **PriorityQueue**: emplace.

## Conforming

Vector (invalidation, growth, relocation with move constructors, aliasing-safe push_back),
String (storage, terminator, searches, comparison), Map/Set/MultiMap/MultiSet (red-black tree,
invalidation, complexity, return values of insert/erase/emplace/try_emplace), FastMap/FastSet
(as specified, including their weaker invalidation rules), sort and stableSort, lowerBound and
upperBound, PriorityQueue.
