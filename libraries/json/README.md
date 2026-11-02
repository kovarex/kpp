# json

JSON ([RFC 8259](https://www.rfc-editor.org/rfc/rfc8259)) for kpp: parse text into a `JsonValue`,
build or change one, and write it back. Strict: text that is not JSON is an error with its line
and column. Numbers round-trip exactly.

```cpp
JsonValue config = parseJson(R"({"name": "kpp", "threads": 32, "tags": ["fast"]})");
println(config["name"].asString());            // kpp
long long threads = config["threads"].asInteger();

JsonValue out;
out.set("version", 1);
out.set("ratio", 0.1);
out.set("tags", JsonValue::array()).push("fast");
println(writeJson(out));                        // {"version":1,"ratio":0.1,"tags":["fast"]}
println(writeJsonPretty(out));                  // two spaces per level
```

## API

| | |
|---|---|
| `JsonValue parseJson(StringView text)` | the one value in the text; throws `JsonParseError` |
| `bool tryParseJson(StringView text, JsonValue& out, JsonParseError* error = nullptr)` | the same without a throw |
| `String writeJson(const JsonValue&)` | compact text |
| `String writeJsonPretty(const JsonValue&, int indent = 2)` | a member or element per line |
| `JsonValue` | `type` (`JsonType::null`, `boolean`, `integer`, `number`, `string`, `array`, `object`) |
| `isNull() isBool() isInteger() isNumber() isString() isArray() isObject()` | `isNumber()` is true for an integer too |
| `asBool() asInteger() asNumber() asString()` | the value; another type throws `JsonTypeError` (`asNumber()` takes an integer too) |
| `size()`, `v[i]`, `v["key"]`, `find(key)`, `contains(key)` | elements and members; `find` gives `nullptr` for a missing member, `v["key"]` throws |
| `push(value)`, `set(key, value)`, `remove(key)` | change an array or object; `push` and `set` turn a null into one |
| `JsonValue::array()`, `JsonValue::object()` | empty ones |
| `==`, `str()` | equality as below; `str()` is `writeJson`, which the test runner shows in a failed check |

A `JsonValue` converts implicitly from `bool`, `int`, `long long`, `double`, `const char*`,
`StringView` and `String`.

`JsonParseError` has `line` and `column` (1-based, the column in bytes) and a message that starts
with them: `2:9: a number with a leading zero`.

## Numbers

- A number written without a fraction or exponent that fits in `long long` is an **integer**
  (`JsonType::integer`), kept exactly. Any other number is a **double** (`JsonType::number`): the
  nearest one, as the C library's `strtod` rounds it. A number too large for a double (`1e400`)
  is an error rather than infinity.
- `-0` is the double -0.0, since an integer cannot hold its sign.
- **Writing** a double gives the shortest of 15, 16 and 17 significant digits that reads back as
  the same double, so `parseJson(writeJson(v)) == v` holds for every finite double, bit for bit
  (tested on 200,000 random bit patterns and the edge cases). It always has a `.` or an
  exponent (`5.0`, `1e+300`), so an integer stays an integer and a double a double through a round
  trip. NaN and infinity have no JSON form: writing one throws `JsonTypeError`.
- `1` and `1.0` are different values (they print differently).

## Choices where the RFC leaves one

- **Text is UTF-8.** A string with bytes that are not UTF-8 (overlong forms, surrogates, past
  U+10FFFF, cut short) is an error, when parsing and when writing. A byte order mark at the start
  is an error (RFC 8259 8.1 says not to add one).
- **Escapes**: `\uD800`-`\uDFFF` must come as a high and low surrogate pair, which becomes one code
  point; a lone surrogate is an error, since UTF-8 cannot hold it. `\u0000` is allowed: a
  `String` holds the zero byte.
- **A repeated key**: the last value wins, in the place of the first.
- **Nesting** deeper than 1000 arrays and objects is an error, not a stack overflow.
- **Objects keep their order**, the order of the text or of `set`. Two objects are equal when
  they have the same members in any order.
- **Writing** escapes `"`, `\` and the control characters below 0x20 (`\n`, `\t`... and `\u00XX`),
  and nothing else: other characters are written as their UTF-8.

## Tests

`kpp --test` in this directory, or `kppc --test libraries/json` anywhere. They include every parsing case
of [JSONTestSuite](https://github.com/nst/JSONTestSuite) (MIT license): its 95 `y_` texts parse,
its 188 `n_` texts do not, and of its 35 `i_` (implementation-defined) texts the six that a
reader can take without changing them parse (huge or tiny numbers, rounded; 500 nested arrays),
while the 29 with lone surrogates, bytes that are not UTF-8, UTF-16, a byte order mark or a number
past a double's range do not. Every text that parses is written and parsed again to the same
value. `tests/JsonTestSuite.kpp` is generated from a clone by `tests/generate_suite.py`.

## Not yet

- Typed (de)serialization of structs by compile-time iteration over their fields
  (docs/library-registry.md, "The website and the library provider in kpp").
- Objects look a member up by a linear search, which is fine for configuration and registry
  files and slow for objects with thousands of members.
- Parsing a stream; the text is one `StringView`.
