# Standard library conformance

The specification of the library is docs/stdlib/ (self-contained: it started from the C++
standard library's guarantees, which converted code relies on, and writes them out). This file
lists where the implementation in stdlib/ does not meet that specification yet. Each item is a bug
to fix, or, if the rule should change instead, a decision to record in the specification and in
design.md. Behaviour that differs silently comes first; API that is missing (a call then fails to
compile, loudly) last. State of 2026-10-29.

## Behaviour that differs

**Compiler gaps that break library use**: one open since 2026-11-01. `new` and `new T[n]` return
null when malloc fails, where C++'s new throws bad_alloc, and new T[n] allocates the wrapped
size when n * sizeof(T) does not fit in a size_t: `new int[2^62 + 1]` gets 4 bytes
(language/new_failure_throws_bad_alloc, pending, B). That reaches UniqueArray and every `new[]` of
the converted game. The four listed until 2026-10-29 pass now (language/brace_argument_binds_rvalue,
templates/forwarding_reference_to_function, language/array_new_and_delete with
library/unique_array_from_new, classes/reference_bound_through_conversion_operator).

**Sizes are int** (core/Sizes.kpp, 2026-11-01). The containers count elements and bytes in int, so
the most any holds is maxContainerSize, 2^31 - 1, where std's max_size() is far larger. Past it,
every path throws LengthError with MSVC's message ("vector too long", "string too long", "map/set
too long", "list too long", "unordered_map/set too long", "deque too long"), as std throws
length_error past max_size(). It throws before an element or byte is copied, and the container is
left as it was.

A block malloc cannot give throws BadAlloc, as std's operator new does. Each path gets the check
it needs:
- a size plus what is added to it goes through sizeSum;
- a count from outside the container (strlen, a pointer range, distance) goes through
  checkedSize;
- doubling stops at maxContainerSize (grownCapacity);
- HashMap and HashSet have at most 2^30 slots, which hold about 805 million entries;
- Deque's map has at most maxContainerSize / blockSize() blocks.

`OutStream << const char*` writes a longer C string in pieces, and String's find and rfind of a
longer needle return npos.

Until 2026-11-01 these sizes wrapped silently:
- String::append's `length + count` passed its capacity check when it wrapped, and the copy
  wrote past the block;
- doubling and `count + 1` went negative, malloc's null was written through;
- WString's `reserve(2^31 - 1)` returned without reserving;
- `(int)strlen` and `int(last - first)` truncated.

The edges are specified in library/size_limits_string, size_limits_vector and size_limits_growth.
stableSort throws BadAlloc when malloc cannot give its buffer. std's stable_sort falls back to
sorting in place instead; that fallback comes when a use needs it.

**Optional** (docs/stdlib/optional.md): not in the specification, kept from the game's own
Optional until nothing uses them: get(), set(), clear(), operator~, emptyValue().

**printf, fprintf, snprintf, sprintf, sscanf**: declared as the UCRT declares them, but the
specification snippets cannot check their types yet: a written variadic function pointer type loses
its `...` (language/variadic_function_pointer_type, pending). 2026-10-30.

**throw_with_nested** (core/Error.kpp): throws its argument without the exception being handled
attached to it, since Kpp has no std::current_exception to capture that one (nor
std::nested_exception and rethrow_if_nested, which no converted code uses). Its one user is
UnitTest++'s Test::HandleDesync, which only needs the RuntimeError it throws. 2026-10-29.

## Missing types and API

None of the members specified in docs/stdlib/ are missing (2026-10-29). One works only for
movable values: HashMap's `emplace(args...)` builds the entry and then moves it into its node
(map.md: constructed in place), so a value that can be neither copied nor moved does not compile
there; operator[] and try_emplace construct it in place as specified. Map, Set, MultiMap and
MultiSet build it in the node since 2026-10-29 (BuildingRenderer's `std::map<RailWithDirection,
RailNode>`, whose MinHeap base deletes its copy).

Not in the library by decision: `<regex>`. Its one user is cxxopts, whose five fixed patterns are
hand-written matchers in the conversion's overrides (docs/path-to-factorio.md, "cxxopts"); a
library Regex comes with a second user.

## Stronger than C++, by decision

**HashMap and HashSet iterators survive growth for dereferencing** (docs/stdlib/hashmap.md,
2026-10-29). std::unordered_map keeps pointers and references valid across a rehash, not
iterators, but MSVC's and libstdc++'s iterators are its nodes and the game relies on that:
NoiseProgramBuilder::compileExpression keeps the iterator `try_emplace` returned while the
expression's compile inserts its subexpressions into the same map, then assigns through it. A
stable table's iterator holds its entry's node (library/hash_map_iterator_after_growth). Moving or
comparing an iterator from before the growth stays invalid, as does any FastHashMap iterator.

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
created type, shared_from_this throwing BadWeakPtr, atomic access), UniquePtr (conversions from UniquePtr<Derived>), UniqueArray (one pointer, its arrays laid out as
`new T[n]` makes them; until 2026-10-29 it was 16 bytes, a pointer and its own count, where
std::unique_ptr<T[]> is 8, which broke the game's size static_asserts), Function (heap copy of the callable,
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
A deferred async's function runs once when several threads get() its SharedFutures at once (until
2026-10-30 two could both take it and the second called it empty, silently on a good run:
library/shared_future_deferred_runs_once_across_threads; the game's PlanetGeneration suite stopped
about 1 run in 10).
