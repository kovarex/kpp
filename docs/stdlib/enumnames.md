# Enum names: snakeName, kebabName

An enum value's name in snake_case (`entity_destroyed`) or kebab-case (`entity-destroyed`), made at compile time from
its enumerators' names (enumerators(E), docs/design.md 8.3), in place of a hand-written switch per enum (kovarex,
2026-11-25: Factorio's Lua API spells names in snake_case, its prototypes in kebab-case). Not in C++.

## The rule

A name is split into words:

- at each `_` (several, or one at the start or the end, separate nothing more);
- before a capital that follows a small letter or a digit (`EntityDestroyed`: entity, destroyed);
- before the last capital of a run of capitals that a small letter follows (`GUIElement`: gui, element;
  `HTTPServer`: http, server).

A digit stays with the word before it (`UTF16LowSurrogate`: utf16, low, surrogate; `Layer2Mask`: layer2, mask). The
words are written in small letters, joined by `_` (snake_case) or `-` (kebab-case). `ENTITY_DESTROYED` and
`EntityDestroyed` both give `entity_destroyed`. Only ASCII letters change.

## Functions

`template<typename E> const char* snakeName(E value)`, `template<typename E> const char* kebabName(E value)`: the
value's name by the rule. The text is constant data, one array per enumerator made at compile time; the call is a
switch on the value, as fast as a hand-written one. Of several enumerators of one value the first names it, as in
the generated `str()`. A value that is no enumerator's aborts, as a switch on a kpp enum does (E is a kpp enum: one
with its values' own brace block, or an `enum class` of kpp source). A name the rule does not give is written before
the call:

```
const char* str() const
{
  if (this == Rails)
    return "rail";
  return std::kebabName(this);
}
```

An enum whose `str()` returns one of these has the generated `fromStr` reading the same names back.

`template<typename E> Optional<E> fromSnakeName(StringView text)`, `fromKebabName`: the value whose snakeName
(kebabName) is text, else none; the enumerators in declaration order, each compared once.

`String snakeCase(const char* name)`, `String kebabCase(const char* name)`: any name written by the rule, at run time
(`snakeCase("readWeight")` is `"read_weight"`).

The pieces, for other names made constant (a reflected method's name in a `for constexpr` body):
`constexpr u64 caseLength(const char* name)` is the length by the rule without the zero; `template<u64 N> struct
CaseText { char chars[N]; }` and `template<u64 N> constexpr CaseText<N> caseText(const char* name, char separator)`
write it, N being `caseLength(name) + 1`:

```
static constexpr std::CaseText<std::caseLength(f.name) + 1> luaName = std::caseText<std::caseLength(f.name) + 1>(f.name, '_');
```

## Cost

Building a name runs in the compile-time evaluator: about 1 ms of CPU per enumerator (2026-11-23,
"Generated enum name strings" study), only for the enums a program calls the functions on.
