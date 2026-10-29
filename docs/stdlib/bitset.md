# Bitset<N>

What `std::bitset<N>` converts to: N bits, stored in 64-bit words inside the object (no heap).
Bit 0 is the lowest. Positions are `int`; a position outside [0, N) is a program error, not
checked (*difference from C++*: `test(pos)` and `set(pos)` there throw out_of_range).

Members:
- `test(pos)`, `operator[](pos)` on a const Bitset: the bit, as a bool.
- `operator[](pos)` on a non-const Bitset: a `reference` to the bit, which reads as a bool
  (implicitly), assigns from a bool or from another reference, has `~` (the bit inverted, as a
  bool) and `flip()`.
- `set()` (every bit), `set(pos)`, `set(pos, value)`, `reset()`, `reset(pos)`, `flip()`,
  `flip(pos)`: each returns the Bitset.
- `count()`: the number of set bits; `size()`: N; `all()`, `any()`, `none()`.
- `&`, `|`, `^`, `~` and `&=`, `|=`, `^=`; `<<`, `>>`, `<<=`, `>>=` by a number of positions, the
  bits shifted out lost and zeros shifted in. No operation leaves a bit at N or above set, so
  `~Bitset<10>()` has 10 bits set, not 64.
- `==`, `!=`.
- `to_ullong()`, `to_ulong()`: the bits as a number; throw OverflowError when a set bit does not
  fit (64 and 32 bits). `to_string(zero = '0', one = '1')`: the bits as text, bit N - 1 first.

`OverflowError` is a RuntimeError (what `std::overflow_error` converts to).
