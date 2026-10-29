# Standard library conformance

The specification of the library is docs/stdlib/ (self-contained: it started from the C++
standard library's guarantees, which converted code relies on, and writes them out). This file
lists where the implementation in stdlib/ does not meet that specification yet. Each item is a bug
to fix, or, if the rule should change instead, a decision to record in the specification and in
design.md. Behaviour that differs silently comes first; API that is missing (a call then fails to
compile, loudly) last. State of 2026-10-29.

## Behaviour that differs

**Compiler gaps that break library use** (pending spec snippets in compiler/tests/spec):
- `map.insert({key, makeUnique<T>()})` picks the `const Pair&` overload over `Pair&&` and then
  cannot copy the move-only value (language/brace_argument_binds_rvalue).
- A function passed by its name to a forwarding reference (`invoke(add, 1, 2)`) does not compile
  (templates/forwarding_reference_to_function); a pointer to it does. Thread takes its function
  and arguments by value, which decays the name to a pointer, so it is not affected.
- `new T[n]` makes one element and `delete[]` destroys one (language/array_new_and_delete): an
  array UniqueArray takes over from `new T[n]` (std::unique_ptr<T[]>(new T[n]), 30 uses in the
  game) is overrun and freed wrongly, silently (library/unique_array_from_new).
- A reference parameter bound through a conversion operator binds to a copy
  (classes/reference_bound_through_conversion_operator): a `ref(x)` argument of a Thread for an
  `int&` parameter reaches a copy of x, silently.

**Optional** (docs/stdlib/optional.md): not in the specification, kept from the game's own
Optional until nothing uses them: get(), set(), clear(), operator~, emptyValue().

## Missing types and API

None of the members specified in docs/stdlib/ are missing (2026-10-28).

Not in the library by decision: `<regex>`. Its one user is cxxopts, whose five fixed patterns are
hand-written matchers in the conversion's overrides (docs/path-to-factorio.md, "cxxopts"); a
library Regex comes with a second user.

## Conforming

Vector (invalidation, growth, relocation with move constructors, aliasing-safe push_back),
String (storage, terminator, searches, comparison), Map/Set/MultiMap/MultiSet (red-black tree,
invalidation, complexity, return values of insert/erase/emplace/try_emplace, comparator parameter
with stateful and transparent comparators), FastMap/FastSet
(as specified, including their weaker invalidation rules), sort and stableSort, lowerBound and
upperBound, PriorityQueue, nthElement (introselect), the checked accessors (`at()` of every container, String's `substr`
and `erase` throw OutOfRange), HashMap/HashSet (entries in nodes, std's interface, hasher and key equality parameters) and
FastHashMap/FastHashSet (as specified), Deque/Queue/Stack (blocks; references survive pushes and
pops at both ends), List, SharedPtr/WeakPtr/EnableSharedFromThis (atomic counts, deletion as the
created type, shared_from_this throwing BadWeakPtr, atomic access), UniquePtr (conversions from UniquePtr<Derived>), Function (heap copy of the callable,
BadFunctionCall on an empty call), Atomic (every size from 1 to 8 bytes moved as bits, so bool,
float, double and enums are exact; Atomic<SharedPtr<T>>). Until 2026-10-28 Atomic of a 1- or 2-byte
type wrote 4 bytes (overwriting the fields after it) and Atomic<float>/<double> converted the value
to an integer; both were silent. RecursiveMutex (re-entrant for its owning thread; until 2026-10-28 it was
Mutex itself, which deadlocks when re-entered). Optional (the value in a union's storage since 2026-10-29: an
empty Optional constructs no T, so T needs no default constructor; until then it held a
default-constructed T). Thread, Thread::Id, thisThread, the mutexes, locks
and ConditionVariable as docs/stdlib/thread.md specifies them (2026-10-29: Thread took only a
callable and joined in its destructor, join and detach did nothing when not joinable, the timed
waits returned a bool and Future's waitFor could report a timeout early on a spurious wake-up).
