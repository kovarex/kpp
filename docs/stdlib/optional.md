# Optional<T>

A value of type T that may be absent ("empty"). The value lives inside the Optional (no heap
allocation).

- An empty Optional holds no value: T's constructor has not run for it and T's destructor will not
  run for it. Giving the value up (`reset()`, `= nullopt`, being destroyed) destroys the value at
  that moment.
- A copy or move of an Optional is empty when the source is, and otherwise copies or moves the
  value; a moved-from Optional keeps its state (present, with a moved-from value).
- An Optional of a trivially copyable T is trivially copyable, and one of a trivially destructible
  T is trivially destructible, so a class of such members keeps those properties.
- The value has T's alignment within the Optional.
- Nothing is thread-safe beyond what T is.

Members:
- `Optional()`, `Optional(nullopt)`: empty. `Optional(const T& value)`, `Optional(T&& value)`:
  holds a copy of value, or value moved in.
- `Optional(U&& value)` for a value of another type a T can be constructed from (not an Optional,
  nullopt or inPlace): holds a T constructed from it; explicit when U does not convert to T
  implicitly (std::optional's converting constructor).
- `Optional(const Optional<U>& other)` for a U that converts to T: empty when other is, otherwise
  holds a T constructed from other's value (`Optional<const T*>` from `Optional<T*>`).
- `Optional(inPlace, args...)` (explicit): holds a T constructed from args in place (std::in_place).
- `= nullopt`: empty; `= value` (copy or move): holds value.
- `= u` for a value of another type T is constructed and assigned from (not an Optional or
  nullopt; std::optional's converting assignment): u is assigned to the value held, or a T is
  constructed from it when empty, through an explicit constructor too (an `Entity*` to an
  `Optional<Targeter<Entity>>`). `= other` for an `Optional<U>` (const& or &&) whose value T is
  constructed and assigned from: empty when other is, otherwise its value assigned or constructed.
- `emplace(args...)`: destroys the value held, if any, then constructs T from args in place;
  returns a reference to it.
- `reset()`: destroys the value held, if any; empty afterwards.
- `has_value()` / `hasValue()`, `explicit operator bool`, `!`.
- `value()`: the value; throws BadOptionalAccess when empty. `*`, `->`: the value; on an empty
  Optional the program is wrong (no check).
- `valueOr(fallback)`: a copy of the value, or fallback when empty. `value_or(fallback)`, std's
  name: the same, with fallback of any type T is constructed from (`p.value_or(nullptr)`).
- `swap(other)`: two values are swapped (`swap` of T); when one Optional is empty, the value is
  moved into it (one move construction) and destroyed in the other; two empty ones stay so.
  noexcept when T's move construction and move assignment are.
- Comparison: `==`, `!=` with another Optional (equal when both are empty or both hold equal
  values), with a value of any type the held one compares with (an empty Optional equals no
  value), with nullopt (equal when empty); `<` and `<=>` with another Optional (empty orders
  before every value, values by their `<=>`, or `<` where they have none) and `<=>` with a value
  (an empty Optional orders before it), which give `>`, `<=` and `>=` too.

`makeOptional(value)`: an Optional holding a copy of value; `makeOptional<T>(args...)`: one holding a
T built from args in place. `BadOptionalAccess`: an Exception, what
value() throws.
