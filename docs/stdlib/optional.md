# Optional<T>

A value of type T that may be absent ("empty"). The value lives inside the Optional (no heap
allocation).

- An empty Optional holds no value: T's constructor has not run for it and T's destructor will not
  run for it. Giving the value up (`reset()`, `= nullopt`, being destroyed) destroys the value at
  that moment.
- A copy or move of an Optional is empty when the source is, and otherwise copies or moves the
  value; a moved-from Optional keeps its state (present, with a moved-from value).
- Nothing is thread-safe beyond what T is.

Members:
- `Optional()`, `Optional(nullopt)`: empty. `Optional(const T& value)`, `Optional(T&& value)`:
  holds a copy of value, or value moved in.
- `= nullopt`: empty; `= value` (copy or move): holds value.
- `emplace(args...)`: destroys the value held, if any, then constructs T from args in place;
  returns a reference to it.
- `reset()`: destroys the value held, if any; empty afterwards.
- `has_value()` / `hasValue()`, `explicit operator bool`, `!`.
- `value()`: the value; throws BadOptionalAccess when empty. `*`, `->`: the value; on an empty
  Optional the program is wrong (no check).
- `valueOr(fallback)`: a copy of the value, or fallback when empty.
- `swap(other)`.
- Comparison: `==`, `!=` with another Optional (equal when both are empty or both hold equal
  values), with a value (an empty Optional equals no value), with nullopt (equal when empty);
  `<` with another Optional: empty orders before every value, values by their `<`.

`makeOptional(value)`: an Optional holding a copy of value. `BadOptionalAccess`: an Exception, what
value() throws.
