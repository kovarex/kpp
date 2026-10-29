# Common rules

These rules hold for every container and algorithm unless its own page says otherwise.

## Sizes and positions

Sizes, counts and indices are `int` (32 bits, signed). A container holds at most 2^31 - 1
elements. *Difference from C++*: C++ uses an unsigned `size_t`, so `size() - 1` on an empty
container is a huge number there and -1 here.

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

Every container names its types as members: `value_type` (the element), `size_type` (`int`),
`difference_type` (`ptrdiff_t`), `reference` and `const_reference`, `pointer` and `const_pointer`,
`iterator` and `const_iterator`, and, where the container has `rbegin()`, `reverse_iterator` and
`const_reverse_iterator`. The maps and sets also have `key_type` and, for maps, `mapped_type`;
the ordered ones `key_compare`, the hash ones `hasher` and `key_equal`. A view (StringView) has
const element types for `reference` and `pointer`.

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
