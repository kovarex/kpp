# Kpp standard library specification

This directory specifies the Kpp standard library: what every type and function does, what it
guarantees and what it costs. It is meant to be complete on its own. It started from the
guarantees of the C++ standard library, because programs converted from C++ rely on them, but it
does not refer to the C++ standard or any other outside text: where Kpp behaves like C++, that
behaviour is written out here in full. When Kpp deliberately differs, the difference is stated
here as the rule, and marked as a difference from C++ so that converted code can be checked.

Where the implementation (stdlib/) does not meet this specification yet, docs/stdlib-conformance.md
lists it. The specification wins: such a gap is a bug to fix, not a change of the rule.

Files:
- [Common rules](common.md): sizes, element requirements, invalidation, complexity, errors.
- [Vector](vector.md), [String](string.md)
- [Map, Set, MultiMap, MultiSet](map.md), [FastMap, FastSet](fastmap.md)
- [HashMap, HashSet](hashmap.md)
- [Deque, Queue, Stack](deque.md), [List, ForwardList](list.md), [PriorityQueue](priorityqueue.md)
- [Algorithms](algorithms.md): sorting and searching ranges.
- [UniquePtr, UniqueArray](uniqueptr.md), [SharedPtr, WeakPtr, EnableSharedFromThis](sharedptr.md)
- [Optional](optional.md), [Function](function.md)
- [Tuple](tuple.md): Tuple, get, tie, makeTuple, forwardAsTuple, TupleSize, IndexSequence, Pair's piecewise
  constructor (std::tuple and its helpers).
- [Thread](thread.md): threads, Thread::Id, thisThread, mutexes, locks and condition variables
  (std::thread, std::mutex, std::unique_lock, std::condition_variable).
- [Atomic](atomic.md): values that threads share without locks; Atomic<SharedPtr<T>>.
- [Limits](limits.md): the properties of the arithmetic types (std::numeric_limits), and Endian (std::endian).
- [Time](time.md): Duration, TimePoint, SteadyClock, SystemClock (std::chrono).
- [Views](views.md): Subrange, iotaView, transformView, reverseView (std::views, std::ranges::subrange).
- [Span](span.md): a view of contiguous elements, of a fixed extent or any length (std::span).
- [Bitset](bitset.md): a fixed number of bits (std::bitset).
- [Filesystem](filesystem.md): paths, files and directories (std::filesystem), and the error codes
  they report with (std::error_code, std::errc, std::system_error).

Not specified yet: Pair beyond what the maps
and Tuple use, Array, StringView, formatting, streams, futures and async, and the C library wrappers
beyond one rule (2026-10-30): every C function the library declares has the type its C header gives
it (the UCRT's for the C runtime, windows.h's for Win32), so that a program importing that header
declares the same function the same way. The C runtime's are global, as in C (`FILE*` streams,
`char*` from strchr, strrchr and strstr); the Win32 ones are in namespace win32 (stdlib/core/Win32.kpp),
called qualified, with windows.h's struct and union tags declared and the library's own storage of
their layout cast to them. Checked by library/c_declarations_as_in_c_headers and
interop/library_win32_declarations_as_in_windows_h.

Changing a rule here is a design decision (docs/design.md section 4): it needs to be explicit,
recorded with its date and reason, and listed as a difference from C++ if it is one.
