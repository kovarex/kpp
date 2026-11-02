# Limits<T>

The properties of an arithmetic type: `Limits<T>::max()`, `Limits<T>::digits`. Defined for every
integer type (signed and unsigned, 8 to 64 bits), the character types (`char`, `signed char`,
`unsigned char`, `wchar_t`, `char16_t`, `char32_t`), `bool`, `float` and `double` (`long double`
is `double`). For any other T (an enumeration, a class) `is_specialized` is false and every value
is zero or false, and every function returns `T()`.

Constants (all `static constexpr`):

| member | integers, characters, bool | float / double |
|---|---|---|
| `is_specialized` | true | true |
| `is_signed` | whether negative values exist (`char` is signed, `wchar_t` is not, `bool` is not) | true |
| `is_integer`, `is_exact` | true | false |
| `has_infinity`, `has_quiet_NaN`, `has_signaling_NaN`, `is_iec559` | false | true |
| `is_bounded` | true | true |
| `is_modulo` | true for the unsigned types except bool (arithmetic wraps around) | false |
| `radix` | 2 | 2 |
| `digits` | bits of the value without the sign (`int`: 31, `uint64_t`: 64, `bool`: 1) | bits of the significand: 24 / 53 |
| `digits10` | decimal digits every value of that many digits survives: floor(digits * log10 2) | 6 / 15 |
| `max_digits10` | 0 | 9 / 17: digits needed to print every value distinctly |
| `min_exponent`, `max_exponent` | 0 | -125, 128 / -1021, 1024 (binary) |
| `min_exponent10`, `max_exponent10` | 0 | -37, 38 / -307, 308 |

Functions (`static`):
- `max()`: the greatest finite value. `min()`: for integers the least value; for floating point the
  least positive normal value. `lowest()`: the least finite value (`-max()` for floating point).
- `epsilon()`: the difference between 1 and the next value (floating point; 0 for integers).
  `round_error()`: 0.5 (floating point), `denorm_min()`: the least positive subnormal value.
- `infinity()`, `quiet_NaN()`, `signaling_NaN()`: those values (floating point; 0 for integers).

All functions except the last three are `constexpr`.

*Differences from C++*: `infinity()`, `quiet_NaN()` and `signaling_NaN()` cannot be used in
constant expressions; `has_denorm`, `has_denorm_loss`, `round_style`, `traps` and
`tinyness_before` are not provided.

## Endian

`Endian` (std::endian) is an enumeration of the byte orders of scalar types: `Endian::little`,
`Endian::big` and `Endian::native`, the order of the target. Every target kpp builds for is
little-endian, so `native` equals `little`; it is a constant, usable in `static_assert`.
