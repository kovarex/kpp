# Common rules

These rules hold for every container and algorithm unless its own page says otherwise.

## Sizes and positions

Sizes, counts and indices are `int` (32 bits, signed). A container holds at most 2^31 - 1
elements. *Difference from C++*: C++ uses an unsigned `size_t`, so `size() - 1` on an empty
container is a huge number there and -1 here.

## Element requirements

An element type needs a destructor and a move constructor; operations that copy elements (copying
a container, `push_back(const T&)`) need a copy constructor, and operations that make new
elements from nothing (`resize(n)`, `map[key]` for a new key) a default constructor.

When a container has to put an existing element somewhere else (growing, shifting, rebalancing),
it **relocates** it: it constructs the element at the new place with the move constructor from the
old one, then destroys the old one. No other operation is used to move elements: assignment
operators are only used where a page says so. A type that keeps its own address in itself, or has
handed it out, can fix that in its move constructor.

Ordered containers and sorting compare with `a < b` (`operator<`), which must be a strict weak
ordering: irreflexive, transitive, and with incomparability transitive. Two values are
**equivalent** when neither is less than the other. Hash containers use `hashOf(value)` and
`a == b`; equal values must have equal hashes.

## Iterators, pointers and references

An iterator, pointer or reference is **invalidated** by an operation when using it afterwards
(dereferencing, comparing, stepping) is undefined. Each container page has a table saying which
operations invalidate what. "End" means the iterator returned by `end()`.

Iterators of every container here are at least *forward*: `*it`, `it->member`, `++it`, `it++`,
`==`, `!=`. *Bidirectional* iterators also have `--it` and `it--`. *Random access* iterators are
pointers (`T*`) with all pointer arithmetic.

## Complexity

Complexities count element operations (constructions, destructions, assignments, comparisons,
hash computations) and allocations, as a function of the container's size n. *Amortized* O(f)
means that any sequence of k such operations costs O(k f) in total, although one of them may cost
more.

## Errors

Calling a function outside its preconditions (an index past the end, `front()` on an empty
container, `*end()`, an iterator of another container) is undefined behaviour: the library does
not check. The exceptions are the checked accessors, which say so on their page (`at()` throws
OutOfRange for an index or key that is not there).

## Copies and moves of containers

Copying a container copies every element (O(n)); the copy is independent of the original.
Moving a container takes its elements in O(1) without touching them; the source is left empty
and usable. Iterators, pointers and references into a moved-from container stay valid and refer
to the same elements, now in the destination (except a String's characters held in place, see
string.md, and end iterators of node-based containers, which belong to the container object).
Swapping two containers exchanges their contents in O(1) with the same rule.
