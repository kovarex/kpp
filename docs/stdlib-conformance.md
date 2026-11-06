# Standard library conformance

The specification of the library is docs/stdlib/ (self-contained: it started from the C++
standard library's guarantees, which converted code relies on, and writes them out). This file
lists where the implementation in stdlib/ does not meet that specification yet. Each item is a bug
to fix, or, if the rule should change instead, a decision to record in the specification and in
design.md. Behaviour that differs silently comes first; API that is missing (a call then fails to
compile, loudly) last. State of 2026-10-29.

## Behaviour that differs

**`::operator new` in an optimized build** (core/Sizes.kpp, 2026-11-06): it is malloc, which an
optimizer may remove when the block does not escape, and its BadAlloc with it: `::operator new(huge)`
whose block nothing keeps returns where C++ (which never elides a call of operator new by name)
throws bad_alloc (library/global_operator_new_and_delete keeps the block).

**A program's replacement of `::operator new`** (core/Sizes.kpp, 2026-11-06): the containers and
strings allocate with malloc, never through the global allocation functions, so a program that
replaces them sees its new-expressions' memory only, where std's allocator calls them for every
container.

**The terminate handler** (core/Error.kpp, 2026-11-04): an exception that ends the program (nobody
catches it, or it leaves a noexcept function) prints its type and message on stderr first, and only
then calls the handler setTerminate installed, where C++'s terminate calls the handler alone (its
default one is what reports). The handler is kpp's own on every target, one for the program as the
standard's is, called by the exception runtime through `kpp_terminate_handler`. A C++ library built
with MSVC that a program links still uses the UCRT's handler (set_terminate, per thread), which
kpp's setTerminate does not install.

**A path is wide text on every target** (core/Filesystem.kpp, 2026-11-04): off Windows std's
`filesystem::path::value_type` is char and its native string the path's bytes; kpp's stays wchar_t
(UTF-32 there), converted to UTF-8 at each system call. A name that is not UTF-8 reads as U+FFFD
where its bytes are not, and is not given back as the same bytes. docs/stdlib/filesystem.md.

**Sizes are unsigned** (core/Sizes.kpp; kovarex 2026-11-01: "all the sizes should be
unsigned"). Every size, count, capacity, length, index and position is a `ContainerSize`, unsigned
and as wide as std's `size_t`: `u64`, the one width (common.md; design.md, "Container sizes are
u64", with kovarex's rule and H's measurement of u32 against u64). From 2026-11-01 until that
decision the sizes were u32, and `kppc --config sizeBits=64` made them u64; the switch is gone and
the option is an error. String is 32 bytes. What stays signed is what can be negative: differences
of positions (`difference_type`, iterator subtraction), iterator offsets, comparison results. The
only deviation left is the limit (common.md): maxContainerSize for every element type, where std's
max_size() depends on it.

`npos` is the largest size, as std's is the largest size_t. A container holds at most one less,
`maxContainerSize`, where std's max_size() is far larger. Past it, every path throws LengthError
with MSVC's message ("vector too long", "string too long", "map/set too long", "list too long",
"unordered_map/set too long", "deque too long"), as std throws length_error past max_size(). It
throws before an element or byte is copied, and the container is left as it was. A negative count
converted to a size is past it, as in C++.

A block malloc cannot give throws BadAlloc, as std's operator new does. Each path gets the check
it needs:
- a size plus what is added to it goes through sizeSum, which never forms a wrapping sum;
- a count from outside the container (strlen, a pointer range, distance) goes through
  checkedSize: LengthError when negative or past the limit;
- doubling stops at maxContainerSize (grownCapacity);
- HashMap and HashSet have at most a power of two of slots, half the size range (2^63), which
  hold 3/4 as many entries;
- Deque's map has at most maxContainerSize / blockSize() blocks.

`OutStream << const char*` writes a longer C string in pieces, and String's find and rfind of a
longer needle return npos.

The unsigned traps in the library's own code, fixed with the change and specified in
library/unsigned_size_traps:
- backward searches stop at 0 instead of testing `i >= 0` (String's, StringView's and WString's
  rfind; WString's find_last_of; Bitset's to_string; PriorityQueue's heapify);
- `length - count` is not formed when count is the larger (WString's find with a longer needle);
- a negative count no longer means "to the end": npos does, as any count past the end
  (substr, erase, append of a part);
- Deque's erase of a range at the front counts down to 0, and an empty range is tested before
  the subtraction;
- PriorityQueue's siftDown loops while a child exists, so `2 * i + 1` cannot wrap;
- compare() results stay int (WString's `<=>` had turned its result unsigned, where `c < 0`
  never holds);
- with u64 sizes no wider type is left to widen a sum into, so String's append compares count with
  the room left instead of forming `length + count`, WString's `reserve(npos)` throws instead of
  wrapping `needed + 1` to 0 (library/size_limits_growth), and HashMap's load checks divide the
  slot count instead of multiplying the entry count.

The index forms that are kpp's own (Vector's `insert(index, value)` and `erase(index)`, String's
`erase(index)`) take an index of any integer type, so that a literal 0 is the index and not a null
position (with a ContainerSize parameter the call was ambiguous).

Until 2026-11-01 the sizes were int, and before that day's checks they wrapped silently:
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

**throw_with_nested** (core/Error.kpp): throws its argument without the exception being handled
attached to it: there is no std::nested_exception to attach it to (nor rethrow_if_nested, which no
converted code uses; currentException can capture it since 2026-11-06). Its one user is
UnitTest++'s Test::HandleDesync, which only needs the RuntimeError it throws. 2026-10-29.

**toString of a floating value is its shortest round-trip text, not `%f`** (docs/stdlib/text.md,
2026-11-01, kovarex: design.md 14). std::to_string(0.1) is `0.100000`; toString(0.1) is `0.1`, and
toString(0.1 + 0.2) `0.30000000000000004`. The conversion is to write `fixed(x, 6)` for a converted
std::to_string of a floating value, C++'s text exactly, with the printf rewrite (design.md 14); until
then the game's 7 functions that do so (all debug drawing and a debug preset file) print the
shortest text.

**String and WString have no `+=`; `<<` appends** (docs/stdlib/text.md, 2026-11-01, kovarex:
design.md 14). The converter writes a std::string's `+=` as `<<`: an integer that C++ appends as one
char as `char(x)`, C text as `StringView(x)`. Not silent: a `+=` left on a String fails to compile.

## Missing types and API

None of the members specified in docs/stdlib/ are missing (2026-10-29). One works only for
movable values: HashMap's `emplace(args...)` builds the entry and then moves it into its node
(map.md: constructed in place), so a value that can be neither copied nor moved does not compile
there; operator[] and try_emplace construct it in place as specified. Map, Set, MultiMap and
MultiSet build it in the node since 2026-10-29 (BuildingRenderer's `std::map<RailWithDirection,
RailNode>`, whose MinHeap base deletes its copy).

Not in the library (time.md has what replaces each): the C++20 calendar and time zones of
`<chrono>` (year_month_day, weekday, the other calendar types and their `/` syntax, time_zone,
zoned_time, utc_clock, tai_clock, gps_clock, clock_cast): kpp's calendar is CivilTime with
toCivilTime and fromCivilTime, in UTC, without leap seconds; FileClock converts with to_sys and
from_sys. The duration literals (`10ms`, `2h`): a duration is written `Milliseconds(10)`.

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
