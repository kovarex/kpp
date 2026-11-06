# Tuple, get, tie, IndexSequence

`Tuple<T1, ..., Tn>` holds n values of the types T1 ... Tn together, n at least 1. It is what a
fixed group of values of different types is returned or stored as: a function's several results
(`return makeTuple(min, max, median);`), a compound Map key. The values live inside the Tuple, in
an order the program does not see (no heap allocation); element i is reached with `get<i>(t)`.

- An element may be a reference (`Tuple<int&, double&>`, what `tie` makes): the Tuple then refers
  to an object it does not own, and must not outlive it.
- A copy or move of a Tuple copies or moves each element. A Tuple of copyable elements is
  copyable; a Tuple with a move-only element (UniquePtr) is only movable.
- Nothing is thread-safe beyond what the elements are.
- Difference from C++: there is no Tuple of no elements (std::tuple<>).

Members:
- `Tuple()`: every element value-initialized (zero for an arithmetic type or a pointer).
- `Tuple(const T1& v1, ..., const Tn& vn)` (implicit): every element copied from its value.
- `Tuple(u1, ..., un)` (implicit, a template): every element constructed from its argument, which
  is forwarded: an rvalue argument is moved from (`Tuple<String, UniquePtr<Sprite>>("name",
  move(sprite))`). Only when there are n arguments and each element can be constructed from its
  argument.
- `Tuple(const Tuple<U1, ..., Un>& other)`, `Tuple(Tuple<U1, ..., Un>&& other)` (implicit): from a
  Tuple of as many elements of other types, each element constructed from the other's, copied or
  moved.
- `Tuple(const Pair<U1, U2>& pair)`, `Tuple(Pair<U1, U2>&& pair)` (implicit, two elements only):
  first and second.
- `= other` (copy or move, from a Tuple of the same or other element types, or for two elements
  from a Pair): assigns element by element. An element that is a reference assigns the object it
  refers to; it is not rebound. So `tie(a, b) = pair` assigns a and b.
- `get<i>()`: element i, what the free get gives for an lvalue Tuple.
- Comparison: `==` (equal when every element is equal to the other's), `!=`, and `<=>`, which
  orders by the first element that differs, by its `<=>`, or its `<` where it has none; this
  gives `<`, `>`, `<=` and `>=` (common.md, "Comparison"). A Tuple of comparable elements can be a
  Map key.

Class template argument deduction: `Tuple(a, b)` and `Tuple{a}` are a Tuple of the arguments'
types, decayed (without reference and const, an array or a function as a pointer); `Tuple(pair)`
is a Tuple of the Pair's two types.

Functions:
- `get<i>(t)`: element i of t, i from 0 to n - 1 (another i does not compile). A reference to it
  for an lvalue Tuple, a const reference for a const one, an rvalue reference for an rvalue one,
  so that `get<0>(move(t))` moves the element out. A reference element is given as the reference
  it is in all three cases.
- `makeTuple(v1, ..., vn)`: a Tuple of the values, of their decayed types, each moved in when it
  is an rvalue; a `ref(x)` argument makes a reference element.
- `tie(a1, ..., an)`: a Tuple of references to the variables, `Tuple<A1&, ..., An&>`; assigning a
  Tuple or a Pair to it assigns the variables.
- `forwardAsTuple(a1, ..., an)`: a Tuple of references to the arguments as they were passed, an
  lvalue reference for an lvalue and an rvalue reference for an rvalue; for passing arguments on
  (Pair's piecewise constructor). It refers to temporaries that end with the full expression.

Tuple-like types: `TupleSize<E>::value` is the number of elements of a tuple-like type E
(`tupleSize<E>` for short) and `TupleElement<i, E>::Type` its element i's type. Tuple and
`Array<T, N>` are tuple-like (an Array has N elements of type T, and `get<i>(array)` is element i
of it). Tuple-like is what a structured binding goes by: `auto [a, b, c] = e` with E tuple-like
binds a, b and c to `get<0>(e)`, `get<1>(e)` and `get<2>(e)` (`e.get<i>()` where E has such a
member), whatever E's data members are. A program may make its own class tuple-like by
specializing TupleSize for it and giving it a get.

Pair is not tuple-like (TupleSize is not specialized for it): a binding of a Pair takes its members
first and second directly. Difference from C++ (std::pair is tuple-like); the bindings are the same
either way.

## Pair

`Pair<A, B>` is std::pair: the public members `first` and `second`. `Pair()` value-initializes both;
`Pair(a, b)` copies or moves each in, and converts from any two values its members can be constructed
from, implicitly when both convert implicitly; `Pair(other)` and `=` from a Pair of other types, member
by member, moving from an rvalue. A reference member assigns what it refers to; assigning one
`Pair<int&, int&>` from another of the same type does not compile (*difference from C++*, as long as
the assignment would make every Pair's non-trivial). `swap(other)`, `swap(a, b)`; `==`, `<=>` and what
they give, first deciding, then second. `makePair(a, b)` stores decayed values, and a ReferenceWrapper's
reference. `Pair(a, b)` deduces both types from the values taken by value.

`get<0>(pair)` and `get<1>(pair)` give first and second, moved from an rvalue Pair; `get<T>(pair)` the
member of type T, for a Pair of two different types.

`Pair(piecewiseConstruct, Tuple<A...> firstArgs, Tuple<B...> secondArgs)`: first constructed from
the elements of firstArgs and second from those of secondArgs, each in place (never copied or
moved afterwards); with forwardAsTuple, from the arguments as they were passed.

`IndexSequence<i1, ..., in>`: a sequence of int constants as a type, for expanding a pack of
indices (`get<I>(t)...`). `MakeIndexSequence<n>` is `IndexSequence<0, 1, ..., n - 1>`, and
`IndexSequenceFor<T...>` is `MakeIndexSequence<sizeof...(T)>`. `IndexSequence<...>::size()` is
the number of its constants.

Costs: a Tuple is as big as a struct of its elements in some order (padding included); get,
construction and comparison cost what the elements' own operations cost, with no more indirection.
