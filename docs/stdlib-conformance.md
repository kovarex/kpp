# Standard library conformance

The specification of the library is docs/stdlib/ (self-contained: it started from the C++
standard library's guarantees, which converted code relies on, and writes them out). This file
lists where the implementation in stdlib/ does not meet that specification yet. Each item is a bug
to fix, or, if the rule should change instead, a decision to record in the specification and in
design.md. Behaviour that differs silently comes first; API that is missing (a call then fails to
compile, loudly) last. State of 2026-10-28.

## Behaviour that differs

**Compiler gaps that break library use** (pending spec snippets in compiler/tests/spec):
- `map.insert({key, makeUnique<T>()})` picks the `const Pair&` overload over `Pair&&` and then
  cannot copy the move-only value (language/brace_argument_binds_rvalue).
- `KeyOf::of(a) < b` with a class type emits invalid IR; Map.kpp and FastMap.kpp avoid the form
  (templates/static_call_through_parameter_operator).
- A parameter pack forwarded into another function template (`inner.emplace(forward<Args>(args)
  ...)`) instantiates it with one parameter of unknown type (templates/
  forward_pack_to_function_template). Queue, Stack and PriorityQueue's emplace therefore build the
  element and move it in (one extra move), and List/ForwardList build theirs in place themselves.

## Missing types and API

None of the members specified in docs/stdlib/ are missing (2026-10-28).

## Conforming

Vector (invalidation, growth, relocation with move constructors, aliasing-safe push_back),
String (storage, terminator, searches, comparison), Map/Set/MultiMap/MultiSet (red-black tree,
invalidation, complexity, return values of insert/erase/emplace/try_emplace), FastMap/FastSet
(as specified, including their weaker invalidation rules), sort and stableSort, lowerBound and
upperBound, PriorityQueue, nthElement (introselect), the checked accessors (`at()` of every container, String's `substr`
and `erase` throw OutOfRange), HashMap/HashSet (entries in nodes, std's interface) and
FastHashMap/FastHashSet (as specified), Deque/Queue/Stack (blocks; references survive pushes and
pops at both ends), List.
