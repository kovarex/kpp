# kpp format (draft)

`kpp format` lays out kpp source in kpp's style: the layout rules of [code-style.md](code-style.md)
(Factorio's style guide carried over to kpp), done by a program. It changes whitespace only. The
decisions behind it are in [design.md](design.md), "kpp format". This page is the exact list of its
rules, each numbered so that a rule can be vetoed by its number, with what each changes on the
converted game and on our own code.

Status (2026-11-09): kpp's default style, with wrapping at 130 columns, and the 13 settings (a project's
`.kppstyle`, `--style`, `--set`). Not built yet: personal styles, the git filter and the language
server's formatting. Nothing has been reformatted (Q9: kovarex checks the counts below first).

## Commands

```
kpp format PATH...                         rewrites the .kpp files (a directory: every .kpp under it)
kpp format --check PATH...                 changes nothing; lists the files not in the style, exit 1 if one is
kpp format --stdout FILE                   writes the file formatted to stdout
kpp format --stats [--examples N] PATH...  changes nothing; counts the changes by rule, with N examples of each
kpp format --style FILE ...                this style for every file, not the project's
kpp format --set KEY=VALUE ...             one setting, over the style's (repeatable)
kpp format --from DIR --to DIR             each .kpp under the first written formatted into the second, at its path
kpp format --perturb SEED --from DIR --to DIR  copies with the whitespace the formatter decides moved (the tests')
kpp format --list-settings                 each setting: its name, its default, its other values
kpp format --columns 0 ...                 no wrapping: the writer's line breaks stay (for the counts)
```

A directory's `build`, `bin` and dot directories are skipped (not under `--from`). Exit codes: 0; 1 when a
file is refused or `--check` finds one not in the style; 2 for a wrong command or style, nothing written.

## Settings

A project's style is the `.kppstyle` beside its build.kpp (the nearest one above the file; a build.kpp
without one is kpp's default): `key = value` lines, `#` starts a comment. A setting not named keeps its
default. An unknown name, a value not listed, or a setting given twice is an error naming the line.

| setting | values (default first) | what it changes |
|---|---|---|
| `indent` | `2`, `4`, `tab` | one level's width; `tab` writes levels as tabs (4 columns each for the limit) and aligns with spaces |
| `columns` | `130`, `100`, `80`, `160` | the limit wrapping works to |
| `braces` | `own-line`, `end-of-line` | where an expanded body's `{` goes (also `case 1: {`). A bare block's stays on its line, one after a `//` comment too; `else` and `catch` stay on the line after the `}` |
| `case-indent` | `yes`, `no` | `case` one level inside its `switch`, or at the switch's column |
| `namespace-indent` | `yes`, `no` | a `namespace`'s contents one level in, or not (`extern "C"` and suites stay indented) |
| `access-label` | `class`, `member` | `public:` at the class's column, or at the members' |
| `wrap-operator` | `end`, `start` | a broken line ends with its operator, or the next starts with it (never a comma or an assignment) |
| `continuation` | `align`, `4`, `8` | a continued line lines up as the rules below say; or goes that many columns in, a wrapped argument list starting on its own line. The `(` of an `if`, `while`, `for`, `switch`, `catch` or `when` stays aligned |
| `pointer` | `type`, `name`, `both` | `T* x`, `T *x` or `T * x` (also `&`, `&&`, `^`); a member pointer's `C::*x` in all three |
| `braced-list-spaces` | `no`, `yes` | `{1, 2}` or `{ 1, 2 }`; `{}` in both |
| `template-space` | `no`, `yes` | `template<typename T>` or `template <typename T>` |
| `initializer-commas` | `leading`, `trailing` | in a list one item to a line, `, b(...)` first on its line, or `a(...),` last on its line and the next item under the first (a comma right after a comment stays first) |
| `blank-lines-between-functions` | `1`, `2` | the empty lines around an expanded definition |

Every value can be undone: formatting a file in any style and then in kpp's default gives the file back.

## How it works

1. The file is lexed by the compiler's lexer. The whitespace and comments between two tokens are a
   *gap*; the formatter writes each gap anew and nothing else.
2. The parser reads the file in its format mode (ParsedFile::formatRoles): every operator and brace
   token gets the role of the first node the parser made at it. That tells `a * b` from `T* p`,
   `x < y` from `Vector<i32>`, a block from a braced list, and so on. A token without a role (in what
   the parser skips) keeps the writer's spacing, counted as "kept as written" (89 gaps in the game's
   1.18 million lines of src and tests). Format mode reads both sides of a `when`, every `if constexpr`
   branch and test code as written.
3. A walk over the tokens gives each one its statement, indentation level, kind of brace and kind of
   colon.
4. Each gap is decided by the rules below, in order: whether a line break comes before the token,
   then the empty lines and the indentation, or the spaces.
5. With a limit, each line's statement is laid out flat. If it fits, the writer's breaks inside it
   go, apart from an empty line. Otherwise it is broken after its loosest operators outside brackets
   (`?` and `:`, then `||`, `&&`, comparisons, `+`, `*`...; an assignment last), else the arguments of
   the bracket that passes the limit go one per line, lined up after its `(` (Q5). Each part that still
   does not fit is broken the same way.
6. The output is lexed again: it must have the input's tokens and comments, else the file is left as
   it is and reported (a formatter bug can refuse a file, never change a program).

A file with a parse error, or with a line splice outside a literal, is not formatted at all (the game: 4
of its 4,916 files have such a splice, 2 of them in Wube's code: PrototypeExplorerGui.kpp and ForkLogic.kpp).

## What the writer keeps

- An empty line (several count as one), in every style, where a rule does not refuse one (rules 7 and
  8, or a gap a rule joins). Inside a statement it is a line break the wrapping cannot remove; next to
  an operator it is before or after it, as `wrap-operator` breaks the line. Before a body's `{` it
  keeps that brace on its own line under `braces = end-of-line`.
- A comment on its own line or at the end of a line.
- A body written on one line (`i32 getX() const { return this.x; }`) or over several.
- An initializer list on one line or over several; within a list over several lines, where its lines
  break.
- A constructor's initializer list or a base clause one item to a line, or not: it is, when its `:` starts
  a line. Written otherwise, it is laid out as the rest of the statement.
- A label's statement on the label's line (`case 1: return 1;`) or on the next.
- A declaration after a template header or a `when`, on the same line or the next.
- `else` or `catch` after a one-liner's `}`, on its line or the next.

Everything else is derived from the tokens. With a limit, the line breaks inside a statement are the
formatter's (rules 19 and 20).

## The rules, with their counts

Measured with `kpp format --stats` in kpp's default style (`--columns 0` for every rule but 19 and 20,
the default limit for those two) on:
- the converted game, factorio-in-kpp 2a57bd0a1 (Factorio 4c178a5b915), read-only: `src` and `tests`
  (Wube's code: 4,574 files, 1,184,674 lines, 2 refused) apart from `libraries` (third-party C and C++
  in their own styles, Lua, zstd, glad...: 342 files, 140,415 lines, 2 refused);
- our code: the 242 .kpp files git tracks under stdlib/, libraries/, programs/, kppnet/ and projectlib/
  (45,935 lines), on master b0221a06.

"Lines" are the input lines a rule changes. An indentation rule counts every line it moves, so a block
indented one level less than its rule wants counts all its lines.

| # | rule | what it does | game src+tests: files | lines | game libraries: lines | our code: files | lines |
|---|---|---|--:|--:|--:|--:|--:|
| 1 | `trailing-space` | no space or tab at a line's end | 9 | 19 | 210 | 0 | 0 |
| 2 | `final-line-end` | the file ends with one line end | 0 | 0 | 0 | 6 | 6 |
| 3 | `tabs` | indentation with spaces, never a tab | 2 | 107 | 7,737 | 0 | 0 |
| 4 | `line-ends` | every line end is the file's (its first line end's, LF or CRLF) | 0 | 0 | 0 | 0 | 0 |
| 5 | `blank-lines-max-one` | never two empty lines in a row | 0 | 0 | 0 | 0 | 0 |
| 6 | `blank-start-of-file` | no empty line or space before the file's first line | 2 | 2 | 0 | 0 | 0 |
| 7 | `blank-after-open-brace` | no empty line after a body's { | 0 | 0 | 0 | 0 | 0 |
| 8 | `blank-before-close-brace` | no empty line before a body's } | 0 | 0 | 0 | 0 | 0 |
| 9 | `blank-around-definition` | an empty line before and after an expanded definition (above its comment) | 84 | 192 | 317 | 0 | 0 |
| 10 | `brace-own-line` | an expanded body's { on a line of its own | 98 | 201 | 5,776 | 3 | 4 |
| 11 | `brace-contents-next-line` | nothing after an expanded body's { on its line | 34 | 60 | 298 | 0 | 0 |
| 12 | `close-brace-own-line` | an expanded body's } first on its line | 38 | 64 | 161 | 0 | 0 |
| 13 | `close-brace-else` | else and catch on the line after a } | 2 | 2 | 514 | 0 | 0 |
| 14 | `close-brace-join` | what ends a statement stays on its line: };  });  } while (x);  and an empty loop body's ; | 50 | 99 | 15 | 0 | 0 |
| 15 | `one-statement-per-line` | each statement or declaration starts a line (a label's may follow it) | 135 | 670 | 1,337 | 3 | 12 |
| 16 | `braceless-body-own-line` | the body of an if, else, for or while without braces on a line of its own | 83 | 435 | 2,275 | 1 | 22 |
| 17 | `else-if-one-line` | else if on one line | 41 | 50 | 15 | 0 | 0 |
| 18 | `access-label-own-line` | public:, protected: and private: on a line of their own | 1 | 1 | 0 | 0 | 0 |
| 19 | `wrap-join` | a statement's lines are joined where they fit within the limit (the writer's breaks are not kept) | 1,555 | 13,339 | 3,017 | 48 | 231 |
| 20 | `wrap-break` | a statement over the limit is broken: after its loosest operators, else one argument per line after its ( | 2,494 | 26,435 | 1,994 | 85 | 760 |
| 21 | `indent-block` | a block's statements one level in | 393 | 5,375 | 28,724 | 2 | 28 |
| 22 | `indent-namespace` | a namespace's declarations one level in | 16 | 263 | 53 | 1 | 50 |
| 23 | `indent-class` | a class's members one level in | 50 | 803 | 1,059 | 1 | 55 |
| 24 | `indent-case` | case labels one level inside their switch, their statements one more | 59 | 641 | 1,644 | 2 | 29 |
| 25 | `indent-access-label` | public:, protected: and private: at the class's column | 21 | 137 | 42 | 1 | 8 |
| 26 | `indent-braceless-body` | a body without braces one level in | 259 | 991 | 3,404 | 2 | 11 |
| 27 | `indent-brace` | a body's { and } at the column of what it belongs to | 321 | 3,586 | 9,474 | 2 | 53 |
| 28 | `indent-continuation-paren` | a continued line inside parentheses: under the start of its argument (a new argument under the first, after the () | 810 | 5,071 | 1,901 | 11 | 16 |
| 29 | `indent-continuation` | another continued line: under what it continues (the value after return or an assignment), else one level in (a method chain's .f()) | 391 | 3,558 | 2,095 | 36 | 197 |
| 30 | `indent-initializer` | a constructor's initializer list or a base clause continued one level in | 62 | 617 | 280 | 1 | 4 |
| 31 | `indent-list` | the lines of an initializer list or an enum one level in | 166 | 2,084 | 1,848 | 2 | 36 |
| 32 | `indent-comment` | a comment on its own line indented as the code after it | 181 | 997 | 2,858 | 7 | 21 |
| 33 | `space-binary-operator` | one space on each side of a binary operator, an assignment, ? and : | 403 | 1,598 | 5,667 | 6 | 100 |
| 34 | `space-unary-operator` | no space after a unary operator | 57 | 101 | 1,154 | 0 | 0 |
| 35 | `space-pointer` | *, &, && and ^ with the type: T* p, const T& r | 299 | 789 | 4,633 | 1 | 1 |
| 36 | `space-template` | no space before or inside template brackets: Vector<i32>, template<typename T> | 113 | 437 | 70 | 2 | 2 |
| 37 | `space-braced-list` | no space inside an initializer list's braces: {1, 2} | 713 | 6,291 | 307 | 0 | 0 |
| 38 | `space-one-liner` | one space inside a one-liner body's braces: { return x; }, and {} when empty | 103 | 178 | 171 | 0 | 0 |
| 39 | `space-before-brace` | one space before a body's { on its line, none between a name and its initializer list | 597 | 3,290 | 77 | 1 | 13 |
| 40 | `space-keyword-paren` | one space between if, for, while, switch, catch, when, requires or return and ( | 31 | 60 | 1,222 | 3 | 17 |
| 41 | `space-call-paren` | no space between a name and the ( of its call or parameters, or the [ of an index | 145 | 412 | 1,226 | 0 | 0 |
| 42 | `space-inside-parens` | no space after ( or [ or before ) or ] | 101 | 983 | 657 | 0 | 0 |
| 43 | `space-comma` | no space before a comma, one after | 208 | 1,660 | 2,219 | 1 | 19 |
| 44 | `space-semicolon` | no space before ;, one after inside for (...) | 92 | 211 | 296 | 0 | 0 |
| 45 | `space-member-access` | no space around ., ->, ?-> and :: | 44 | 168 | 2 | 0 | 0 |
| 46 | `space-trailing-comment` | one space before a comment at a line's end | 110 | 854 | 237 | 25 | 95 |
| 47 | `space-between-words` | one space between two names, keywords or literals | 302 | 793 | 898 | 0 | 0 |
| 48 | `space-label-colon` | no space before the colon of a label, a case or an access label | 48 | 271 | 189 | 0 | 0 |
| 49 | `space-other` | the rest of the spacing rules (operator names, ..., lambda captures) | 58 | 481 | 3,449 | 1 | 23 |
| 50 | `initializer-list-lines` | a constructor's initializer list or a base clause whose : starts a line: one item to a line, its commas first (initializer-commas) | 45 | 71 | 1 | 6 | 12 |

### In total

| corpus | files changed | lines changed, no limit | with 130 columns | gaps changed (130) |
|---|--:|--:|--:|--:|
| game src+tests (4,574 files) | 2,312 (3,230 with 130) | 39,165 (3.3%) | 70,898 (6.0%) | 161,822 |
| game libraries (342 files) | 237 (245 with 130) | 71,190 (50.7%) | 71,627 (51.0%) | 134,451 |
| our code (242 files) | 85 (123 with 130) | 806 (1.8%) | 1,503 (3.3%) | 3,512 |

By class, on the game's src and tests without a limit (lines, summed over the rules, so a line two
rules change counts twice): indentation (21 to 32) 24,123; spaces (33 to 49) 18,577; braces and
statements on lines (10 to 18) 1,582; empty lines (5 to 9) 194; trailing spaces, tabs, line ends (1
to 4) 126.

The largest, on the game's src and tests:
- **37 `space-braced-list`, 6,291 lines**: the game writes `{ 1, 2 }` (Q2 chose `{1, 2}`).
- **21 `indent-block`, 5,375; 27 `indent-brace`, 3,586**: blocks written at another level than their
  rule's, each counting all its lines.
- **28 `indent-continuation-paren`, 5,071; 29 `indent-continuation`, 3,558**: continued lines that no
  longer line up because the converter changed the text before them and kept the old columns (C++'s
  `std::find_if(` is kpp's `::findIf(`, four characters shorter), and lines the writer lined up under
  something else (a `?`, another string).
- **39 `space-before-brace`, 3,290**: alignment before a one-liner's `{` (`f()       { ... }`) and
  `T name { 1, 2 }` (the default writes `T name{1, 2}`).
- **15 `one-statement-per-line`, 670 lines and 11,482 statements**: mostly converted macros that wrote
  a whole switch on one line (`case A: return "a"; case B: return "b"; ...`); they become one statement
  a line.
- **46 `space-trailing-comment`, 854**: comments aligned in a column; the default aligns nothing.

With the limit, 19 `wrap-join` joins 13,339 lines of 1,555 files (statements the writer broke that fit
in 130 columns) and 20 `wrap-break` breaks 26,435 lines of 2,494 files.

Our own code changes little: 46 (aligned trailing comments in kppnet), 29 (lines continued one level in
rather than under what they continue), 22/23 (GDI+'s nested namespace written unindented), 33 (Math.kpp's
`0.5*f*f`).

### Speed

`kpp format --check` over the game's 4,916 files: 1.2 s (32 threads, right after a reboot). Formatting the result
again changes nothing, on the whole game (4,912 files) and on our code.

## Tests

`compiler\tests\format.ps1` (run_tests' "format"): `tests\format\*.kpp` are in the style and come back
unchanged; `tests\format\fix\NAME.kpp` formats to `NAME.expected`, which formatting leaves as it is;
`--check` exits 0 and 1 as it should.

`compiler\tests\format\run_format.py` (test-suites' runner) tests each configuration: kpp's default, each
other value of each setting, and `combinations\NAME.kppstyle`, the settings that interact (`kr`, `narrow`,
`spaces`, `tabs-continuation`, `tabs-initializers`): 24 in all. For each file: back to the default gives
the file, formatting twice is formatting once, perturbed copies format the same, each file keeps its line
ends, `--check` agrees. `settings\NAME.kpp` in each value is `settings\NAME.VALUE.expected`. It passes on
the snippets, our code and slices of the game (576 files, every 25th at three offsets).
