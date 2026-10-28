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

Not specified yet: Optional, Function, Tuple, Pair beyond what the
maps use, Span, Array, StringView, formatting, streams, threads, time, and the C library wrappers.

Changing a rule here is a design decision (docs/design.md section 4): it needs to be explicit,
recorded with its date and reason, and listed as a difference from C++ if it is one.
