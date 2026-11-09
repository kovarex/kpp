# Common rules

These rules hold for every container and algorithm unless its own page says otherwise.

## Sizes and positions

Sizes, counts, capacities, lengths, indices and positions are `ContainerSize`: unsigned, as C++'s
`size_t`, so `size() - 1` on an empty container is the largest size here as there, and a negative
`int` passed as a count becomes a huge one. It is `u64`, as wide as `size_t` (stdlib/core/Sizes.kpp;
design.md, "Container sizes are u64": one width, so that there is one String and one Vector).
`npos` is its largest value; a container holds at most one less, `maxContainerSize` (2^64 - 2). A
size past that throws LengthError (C++'s length_error past max_size()), and memory that cannot be
had throws BadAlloc, as does a count whose bytes a `size_t` cannot count.

What stays signed is what can be negative: a difference of positions (`difference_type`,
`ptrdiff_t`, an iterator's `-`), an iterator's offset, and a comparison's result (`compare`).

*Difference from C++*: the limit. std's max_size() depends on the element (MSVC's vector: PTRDIFF_MAX
/ sizeof(T)), and a count past it throws length_error; here the limit is maxContainerSize for every
element type, and a count of elements whose bytes no allocation can hold throws BadAlloc.

## Allocation functions

`::operator new(n)` and `operator new[](n)` give a block of n bytes (at least one), and throw
BadAlloc when it cannot be had; `::operator delete(p)` and `operator delete[](p)`, with or without
the size after p, give it back, nothing for null; new[] is new, a sized one the unsized one, delete[]
delete. A program may replace any of them, and new and delete then call them (design.md,
"Allocation functions"). *Difference from C++*: the library's containers and strings allocate with
malloc, never through them, so a replacement does not see their memory.

## Element requirements

An element type needs a destructor and a move constructor; operations that copy elements (copying
a container, `push_back(const T&)`) need a copy constructor, and operations that make new
elements from nothing (`resize(n)`, `map[key]` for a new key) a default constructor. The
exceptions are the operations that construct an element in its place and never move it after,
which need no move constructor: those of Map and HashMap that say so (operator[], emplace,
try_emplace).

When a container has to put an existing element somewhere else (growing, shifting, rebalancing),
it **relocates** it: it constructs the element at the new place with the move constructor from the
old one, then destroys the old one. No other operation is used to move elements: assignment
operators are only used where a page says so. A type that keeps its own address in itself, or has
handed it out, can fix that in its move constructor.

Ordered containers and sorting compare with a **comparator**: a function object `comp` with
`comp(a, b)` meaning "a is less than b". It is `Less<T>` unless given, which is `a < b`
(`operator<`). It must be a strict weak ordering: irreflexive, transitive, and with
incomparability transitive. Two values are **equivalent** when neither is less than the other.
Hash containers use a **hasher**, `Hash<T>` unless given, which calls the `hashOf(value)`
overload for the type, and a **key equality**, `EqualTo<T>` unless given, which is `a == b`;
equal values must have equal hashes. A container keeps a copy of its comparator (or hasher and
key equality), made when the container is constructed and copied along with it, so comparators
that carry state work.

The function objects: `Less<T>` (`a < b`), `Greater<T>` (`b < a`), `EqualTo<T>` (`a == b`) and
`Hash<T>` (`hashOf(a)`). `Less<>`, `Greater<>` and `EqualTo<>` (T left at its default, void)
take operands of any two types and are **transparent**: they declare a member type
`is_transparent`. When an ordered container's comparator is transparent (any comparator that
declares `is_transparent`; `isTransparent<C>` tells), its lookups (find, contains, count,
lowerBound, upperBound, equal_range) take a key of any type the comparator accepts and compare it
as it is; otherwise a key of another type is converted to the key type once, before the search.

## Member types

Every container names its types as members: `value_type` (the element), `size_type` (`ContainerSize`),
`difference_type` (`ptrdiff_t`), `reference` and `const_reference`, `pointer` and `const_pointer`,
`iterator` and `const_iterator`, and, where the container has `rbegin()`, `reverse_iterator` and
`const_reverse_iterator`. The maps and sets also have `key_type` and, for maps, `mapped_type`;
the ordered ones `key_compare`, the hash ones `hasher` and `key_equal`. A view (StringView) has
const element types for `reference` and `pointer`.

A const container hands out no mutable element, as C++'s: its `begin()`, `end()`, `find()` and the
other positions are `const_iterator`s (whose `*it` is const), and `front()`, `back()`, `[]` and
`at()` return const references. A mutable container's positions are `iterator`s, which convert
implicitly to `const_iterator`s (they compare equal to the same position, and `erase`, `insert`
with a hint and `splice` take either). A set's elements are its keys: its `iterator` is a
`const_iterator` as well.

## Iterators, pointers and references

An iterator, pointer or reference is **invalidated** by an operation when using it afterwards
(dereferencing, comparing, stepping) is undefined. Each container page has a table saying which
operations invalidate what. "End" means the iterator returned by `end()`.

Iterators of every container here are at least *forward*: `*it`, `it->member`, `++it`, `it++`,
`==`, `!=`. *Bidirectional* iterators also have `--it` and `it--`. *Random access* iterators are
pointers (`T*`) with all pointer arithmetic. Every container (and String, StringView, WString,
Span) has `cbegin()` and `cend()`, which are `begin()` and `end()` of it as const, and those with
`rbegin()` have `crbegin()` and `crend()` the same way. The free forms, as std::begin and the rest:
`begin(c)`, `end(c)`, `cbegin(c)`, `cend(c)`, `rbegin(c)`, `rend(c)`, `size(c)`, `ssize(c)` of a
container or a built-in array (`T (&)[N]`: pointers, and N), `empty(c)` and `data(c)` of a container.

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

## Comparison

The sequences (Vector, Deque, List, ForwardList, Array, String, StringView, WString), the ordered
containers (Map, Set, MultiMap, MultiSet, FastMap, FastSet), Pair and Tuple compare with `==` and
`<=>`:
- `a == b`: the same number of elements, equal one by one in order by `==`. `a != b` is its
  negation.
- `a <=> b`: lexicographically. The first pair of elements that differ decides, by their `<=>`
  where they have one and by `<` otherwise (less when `x < y`, greater when `y < x`, else
  equivalent); when one runs out first, it is the lesser. `a < b` is `(a <=> b) < 0`, and so are
  `>`, `<=` and `>=`, so a class whose `<=>` is defaulted can have such a member.

The hash containers (HashMap, HashSet, FastHashMap, FastHashSet) compare with `==` only: the same
size, and every entry of one is in the other (a HashMap's with an equal value), in any order.
Optional's comparisons are on its page.

## Copies and moves of containers

Copying a container copies every element (O(n)); the copy is independent of the original.
Moving a container takes its elements in O(1) without touching them; the source is left empty
and usable. Iterators, pointers and references into a moved-from container stay valid and refer
to the same elements, now in the destination (except a String's characters held in place, see
string.md, and end iterators of node-based containers, which belong to the container object).
Swapping two containers exchanges their contents in O(1) with the same rule.

## What throws nothing

Where std's functions are noexcept, kpp's are, so the noexcept operator and the isNothrow* traits
answer as C++'s (`noexcept(T(move(x)))` is T's move constructor's answer):
- move, forward, declval, asConst and isConstantEvaluated always.
- `swap(a, b)` when T's move construction and move assignment throw nothing; `exchange(slot, v)`
  when T's move construction and the assignment from v do.
- A container's `swap(other)`: a sequence's (Vector, Deque, List, ForwardList), String's,
  StringView's, FastMap's, FastSet's and the smart pointers' always; Map's, Set's, MultiMap's and
  MultiSet's when a copy of the comparator throws nothing, HashMap's and HashSet's when copies of the
  hasher and the key equality do; Array's, Optional's and Pair's when the elements' move construction
  and move assignment do, PriorityQueue's when its container's and comparator's do.
- A Tuple's move assignment when each element's does (a reference element's: the assignment of what
  it refers to, a copy for an lvalue reference).
