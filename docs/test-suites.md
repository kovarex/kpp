# Native test suites, fixtures, dependencies and checks

The survey behind design.md 8.7 and the converter's mapping. Written 2026-11-06 as a proposal;
kovarex decided every question the same day (section f), and design.md 8.7 is the rule. Counts:
kpp b574b288, Factorio 4c178a5b915, factorio-in-kpp 14ce2fbb. Owners: the language and the runner
(parser, compiler/src/tests.cpp), the converter's mapping (thread C).

## (a) What kpp has today

Section 8.6 of design.md, implemented 2026-10-30 (`compiler/src/tests.cpp`, `parser.cpp` 3767 and 4460):

- `test "name" { ... }` at namespace scope or inside a class, where it sees private members and
  is the class's static function. It is compiled only with `kppc --test ROOTS [-- runner args]`
  (build and run) or `--tests --exe` (build only). In a test build the program's `main` is left
  out and `config::tests` is true.
- `check(expr)` records a failure and **continues**. `require(expr)` records it and **ends the
  test** by throwing, so destructors run. Both are statements only inside a test's body; elsewhere
  `check` is an ordinary name. The message is in the compiler's error format, with both operand
  values for a top-level comparison (`==`, `!=`, `<`, `<=`, `>`, `>=`) when `toString` or `str()`
  can show them:
  `square.kpp:7:3: check failed: square(-2) == 5 (4 == 5)`.
- A failing `assert` or `release_assert` inside a test fails the test, and so does an exception
  that leaves it (`test failed: an exception left the test: <what()>`).
- The runner runs the tests in one process, in registration order. Its options are
  `--filter TEXT` (a part of `file > name`, `*` matching anything, repeatable), `--list` and
  `--report FILE` (one JSON line per test). It prints `failed: file > name` after each failed
  test and `N tests, M failed` at the end, and exits with 0 or 1.
- Not yet: suites, fixtures (8.6 says "ordinary classes constructed in the test"), dependencies,
  ordering, a compile error for duplicate names, `--jobs N` (Q3), a failure message argument, a
  check of a throw, an unconditional failure, `check` outside a test body.
- design.md "Running and testing kpp code" Q4 proposed leaving the game's tests on UnitTest++ for
  now. This proposal replaces that answer.
- Spec snippets: `compiler/tests/spec/kpp/test_*.kpp` and `tests_in_compiler.kpp`.

## (b) Factorio's UnitTest++ fork

Source: `C:\Factorio\libraries\UnitTest++` (2,248 lines), plus the game's runner in `C:\Factorio\tests`
(Main.cpp, TestRun.cpp, TestRunner.cpp, TestSelector.cpp, ForkLogic.cpp).

**Declarations** (`TestMacros.h`), counted at the start of a line in tests/ and src/ (src/ has none):

| Macro | Uses | What it expands to |
|---|---|---|
| `SUITE(Name)` | 562 | `namespace SuiteName { currentSuite = SuiteList.create(LowercaseString("Name")) } namespace SuiteName { ... }` |
| `TEST(Name)` | 7,648 | `TEST_FIXTURE_EX(UnitTest::, NullFixture, , Name, TestList)` |
| `TEST_FIXTURE(Fixture, Name)` | 1,186 (51 fixture classes; GameViewFixtureWithAudio 324, GameViewFixture 124, LatencyInputFixture 93) | a helper class deriving from Fixture whose `Run()` is the body, so the body uses the fixture's members unqualified |
| `TEST_PARAMETRISED_FIXTURE(Fixture, Arg, Name)` | 131 | the same, with `Fixture(Arg)` as the constructor call |
| `DEPENDENCY(Name)` | 69, in 48 suites | `SuiteDependencyAdder NameDependency(currentSuite, "Name")` |
| `SUITE_MODPACKS("vanilla")` | 4 | the suite runs only under `--modpack vanilla`; a suite without it runs only under `default` |

**Dependencies are between suites, not tests** (`Suite.cpp`, `SuiteList.cpp:24-37`):

- `SuiteList::setup` resolves the names. An unknown name or a duplicate suite is a runtime
  `runtime_error`. It then gives each suite a `topologyOrder` by DFS, starting from the suites
  without dependencies in map order (lower case alphabetical), with dependencies first.
- `TestList::sort` (`Test::operator<`, `Test.cpp`) sorts by the suite's topology order, then by
  line number, then by test name, then by file name. So all of a dependency's tests run before
  any test of a suite that depends on it.
- **A failed dependency does not skip its dependents.** The order is the only effect: simple
  failures show up first.
- Selection (`TestSelector.cpp:44-66`): with `includeDependencies` (set from the interactive
  selector, `TestSelectionResult.cpp:71`), choosing a suite also runs every suite it depends on.
- There is no cycle check. Cyclic suites would never get an order from the DFS's roots.

**Names:** suite and test names are `LowercaseString` (case-insensitive). Test names must be
unique across the whole program, not only within a suite (`TestList::Add`: "all test names must
be unique"), because `--test NAME` selects by bare name.

**Runner command line** (`Main.cpp:141-200`):
- selection: `--test/-t` (repeatable), `--tests CSV`, `--tests-file`, `--suite/-s`, `--suites`,
  `--skip-suites`, `--from`, `--after`, `--contains`, `--test-contains`, `--suite-contains`,
  `-V` (invert);
- running: `--exit-on-failure`, `--randomize-tests-order`, `--repeat-count`, `--fork` and
  `--num-forks` (one process per suite, `ForkLogic.cpp:229-275`), `--modpack`;
- reporting: `--xml`, a 5-minute slow-test warning per test.

The game's own state (`TestGlobals`: heavy mode, CRC mode, desync reports) is set beside these.

**Fixtures:** constructed before the body and destroyed after it. An exception in either step is
reported as "creating fixture" or "tearing down fixture" (`Test.cpp:30-63`). A
`LatencyTestDesyncError` from the body re-runs the test to write a desync report (`HandleDesync`).

**Generated tests** (`tests/GeneratedTests.hpp`): 13 `TEST_GENERATOR(Name)` blocks (TestedSaves,
ChangeForceOf, EntityMatrixBuildAndDestroy, MainMenuSimulations, TipsAndTricksSimulations, ...)
add tests when the runner starts (`TestGeneratorsList::runAllTestGenerators`, `tests/Main.cpp:365`)
with `addTest(nameSuffix, std::function<void()>)`: one per save file, per entity pair. They have
line 0, so they sort first in their suite.

**Checks** (`CheckMacros.h`, `Checks.h`). Uses in tests/ `.cpp` files, 52,089 in all:

| Macro | Uses |
|---|---|
| `CHECK_EQUAL` | 23,696 |
| `CHECK` | 23,421 |
| `CHECK_CLOSE` | 959 |
| `CHECK_SIGNED` / `CHECK_EQUAL_SIGNED` (an id printed with the failure) | 444 / 436 |
| `CHECK_BIGGER` / `_BIGGEREQUAL` / `_SMALLER` / `_SMALLEREQUAL` | 299 / 67 / 163 / 45 |
| `CHECK_NOT_NULL` / `CHECK_NULL` | 270 / 186 |
| `CHECK_THROW` / `_THROW_EQUAL` / `_THROW_CONTAINS` | 123 / 9 / 8 |
| `CHECK_UNEQUAL` | 85 |
| `CHECK_FAILED` (an unconditional failure with a streamed message) | 67 (+8 `_SIGNED`, +5 `_SOURCELOCATION`) |
| `CHECK_CONTAINS` / `_STARTS_WITH` / `_DOES_NOT_START_WITH` | 31 / 21 / 2 |
| `CHECK_CLOSE_PERCENTAGE`, `_BETWEEN`, `_BETWEENEQUAL`, `_NOT_CLOSE`, `_ARRAY_EQUAL` | 29, 12, 13, 5, 5 |
| `*_SOURCELOCATION` variants (a helper passes its caller's `std::source_location`) | about 50 |

- The tests define 24 check macros of their own, among them `CHECK_LUA_NUMBER_EQUALS` 808,
  `CHECK_BUILDABILITY` 722 (+102 `_COLLIDER`, +25 `_FORCED`), `CHECK_SORT`, `CHECK_CRC`,
  `CHECK_DAMAGED` and `CHECK_MOVE_RESULT`.
- Checks also sit outside test bodies: 135 in 14 tests/ headers (fixtures and helpers) and some
  in 17 non-Test .cpp files.
- No `REQUIRE`: 0 uses.

**Behaviour that matters for the mapping:**

1. **In Factorio's fork, every failed check ends the test.** `TestResults::onTestFailure` throws
   `TerminateTestException` (`TestResults.cpp:33`). So a Factorio `CHECK` behaves like kpp's
   `require`, not like kpp's `check`. Tests rely on this: for example, `CHECK_NOT_NULL(p);`
   followed by `p->x`.
2. **`CHECK_EQUAL` on integers of different signedness compares the values** (`Checks.h:32-55`):
   a negative signed value never equals an unsigned one. kpp's `==` converts the signed side, as
   C++ does (design.md:1029).
3. **`CHECK_EQUAL(const char*, const char*)` compares the text** (`Checks.h:189`, `strcmp`), not
   the pointers.
4. **`CHECK_EQUAL(actual, expected)` evaluates `actual == expected`.** The failure says
   `Expected <e> but was <a>`. An exception inside a check is reported as "Unhandled exception
   in check macro: ..." and ends the test.

## (c) Proposed syntax

### Suites and dependencies

A suite groups tests and orders them. **Its name is an identifier, in a namespace of its own**: suite
names are found only where a suite name is expected (after `suite` and `depends`, and in the
runner's names), never by ordinary lookup, as C keeps struct tags apart from other names. That
matters: 340 of Factorio's 562 suite names are also game class names (Beacon, Animation,
AlertCenter, ...), and `suite Beacon` must not hide `class Beacon` from `Beacon* beacon` inside it.
The suite's declarations (helper functions and classes, which C++ wrote inside `namespace SuiteX`)
live in a scope of their own, private to the suite. `depends` names other suites; the resolver
checks them like any other name (and an editor can go to them and rename them).

**A test's name is an identifier, with no string form** (decided, kovarex 2026-11-06): one
spelling, `test BeaconEffect`. Factorio's 8,965 test names are all identifiers already; kpp's
existing quoted names move to identifiers (see "Migration" below).

```
suite DragBuildingBelt depends DragBuilding, TransportBelt
{
  test DragBuildBeltGhostForwardsOverObstacleConnectingToABelt
  {
    GameViewFixtureForBelts fixture;
    ...
    fixture.checkEntities(1, "<<<w)v(w");
  }
}
```

- These are compile errors: an unknown suite in `depends`, a dependency cycle (the error names
  the cycle), and two suites or two tests of one name.
- Suite and test names are ordinary case-sensitive identifiers, as every kpp name is (kovarex,
  2026-11-06): `suite Beacon` and `suite beacon` are two suites, and `depends beacon` does not
  find `Beacon`. Only the runner's command-line matching (`--filter`, `--suite`, `--skip-suite`,
  `--from`, `--after`) ignores case, a convenience matching Factorio's command line. The converter
  is unaffected: all 41 suite names that Factorio's 69 `DEPENDENCY(X)` lines name are spelled
  exactly as their `SUITE(X)`, and no two of the 562 suites differ only in case.
- Test names are scoped per suite (decided, Q7): two suites may each have a test called `Empty`.
  A test's full name is `Suite.Test` (`Beacon.BeaconEffect`), used by every report line,
  `--list`, `--report` and `--filter`.
  A test outside any suite keeps today's `file > test`, so existing kpp tests are unchanged.
- A suite is one block in one file: a second `suite X` is a compile error naming both places,
  as Factorio's `SuiteList::create` throws "Duplicate suite definition" (no Factorio suite spans
  two files, and no file declares two).
- `depends` on a single test (`test x depends Suite.y`) is left out until something needs
  it: Factorio has none.
- A suite tag replaces `SUITE_MODPACKS`: `suite Vanilla tags vanilla { }`. With no
  `--tag`, the runner runs the untagged suites. With `--tag vanilla`, it runs the suites tagged
  `vanilla`. This is Factorio's modpack rule with "default" meaning untagged, and it is not
  specific to the game.

### Fixtures

The fixture follows the test's name, like a base class. The body is a member function of a class
derived from the fixture, so it uses the fixture's members unqualified, as the C++ did. Arguments
make it parametrised.

```
suite Achievements
{
  test Achievements_IsSimulation : AchievementFixture("eco-unfriendly")
  {
    scenario.update();                          // AchievementFixture::scenario
    require(!achievementID.isUnlocked());
  }
}
```

- The fixture is constructed before the body and destroyed after it, even when the body fails.
- A failure or exception while constructing it is `test failed: creating fixture AchievementFixture: <what>`,
  and the body does not run. One while destroying it is `...: tearing down fixture ...`.
- A fixture is an ordinary class; nothing marks it.

### TestBeacon converted

Today: `tests/TestBeacon.kpp` in factorio-in-kpp, line 1 and following. Each test is one line of
expanded macro classes (`class NullFixtureBeaconEffectHelper : public UnitTest:: NullFixture,
public UnitTest::FixtureHelperBase { ... } ... UnitTest::ListAdder adderNullFixtureBeaconEffect{...};
void NullFixtureBeaconEffectHelper::Run()`), and each check is a call of
`UnitTest::checkInternal([&](UnitTest::MemoryOutStream& CHECK_INTERNAL_stream) -> bool { return
(UnitTest::CheckEqual(beacon->getSource().receivers.size(), 2u, CHECK_INTERNAL_stream)); },
("TestBeacon.cpp"), (48));` (TestBeacon.kpp:26). Proposed:

```
suite Beacon
{
  // The main purpose of this test is, to test different order
  // of setup calls (first assembling machine or first beacon)
  // Mainly because the different orders might affect the electricity setups
  test BeaconEffect
  {
    TestScenario scenario;
    scenario.addEntity("substation", MapPosition(0, 0));
    ...
    Beacon* beacon = scenario.addEntity("beacon", MapPosition(3.5, 0.5));
    TestScenario::transferItems(beacon, "speed-module-3", 2);
    require(beacon->getSource().receivers.size() == 2u);
    scenario.update(100);
    require(assemblingMachine1->getElectricEnergySource()->buffer.isFull());
    require(assemblingMachine2->getElectricEnergySource()->buffer.isFull());
    scenario.update();
  }

  test BeaconOrderAndCancelDeconstruction
  {
    ...
  }
}
```

The file shrinks from 16 lines of generated classes and about 60 lambda-wrapped checks to the C++
source's own shape. The `require` above follows Q1 (decided: every `CHECK*` becomes `require`).

### Checks

| Form | Meaning |
|---|---|
| `check(expr)` | record a failure, continue (today) |
| `require(expr)` | record a failure, end the test (today) |
| `check(expr, args...)` / `require(expr, args...)` | **new**: the arguments are printed after the failure, as `println` prints them (replaces `_SIGNED`'s id) |
| `fail(args...)` | **new**: an unconditional failure that ends the test (replaces `CHECK_FAILED`) |
| `requireThrows<T>(expr)` | **new**: evaluates `expr`, unevaluated until then, inside a try. It fails and ends the test unless a `T` (or a class derived from it) is thrown, and returns the caught exception, so `requireThrows<RuntimeError>(f()).what() == "x"` can be checked next. `checkThrows<T>(expr)` returns an `Optional<T>`. |

**What a failure shows, without macros.** The compiler sees the expression, so it already knows
the location and the text, as today. The values extend today's operand values for a top-level
comparison:

- a top-level call shows its arguments' values:
  `check(isClose(x, 1.5, 0.01))` → `check failed: isClose(x, 1.5, 0.01) (x = 1.52, 1.5, 0.01)`;
- `!call` shows the same;
- `ptr == nullptr` shows the pointer.

So `CHECK_CLOSE`, `CHECK_BETWEEN`, `CHECK_CONTAINS` and `CHECK_STARTS_WITH` become calls of
ordinary stdlib or test functions, and their values still print. No macro and no
`MemoryOutStream` are involved: the runner formats the values with `toString`/`str()` (today's
`__kppShow`, tests.cpp:71).

**A comparison inside `check` and `require` compares integer values exactly.** A signed and an
unsigned operand compare as numbers, as `std::cmp_equal` does: `check(-1 == 4294967295u)` fails.
This is the only place where a check differs from the same expression outside it, and it is what
a test means. It is also what `CHECK_EQUAL` did. Decided in Q4.

**Checks outside a test body.** A function or class declared `test` (`test void
checkBuildability(...)`, `test class GameViewFixture { ... }`) is compiled only in a test build
and may use `check`, `require` and `fail`. A failure reports the check's own location and then
one `called from` line for each test-function frame up to the test:

```
GameViewFixture.kpp:212:5: check failed: entity->name == expected (inserter == fast-inserter)
TestInserter.kpp:48:3: called from here
```

This replaces the `*_SOURCELOCATION` variants and the custom check macros' reliance on `__LINE__`.
Each `test` function has a hidden `CallerLocation` parameter, filled at the call site. A lambda
inside a test body is part of the body: `check` already parses there, and a spec snippet should
confirm it.

**Does a failed check continue?** `check` continues and `require` ends the test, as decided in
8.6. UnitTest++ in Factorio's fork has only "ends the test" (finding 1 above).

## (d) The runner

**Order:**
1. Suites in topological order, dependencies first. Ties are broken by the suite name
   (Kahn's algorithm with a sorted queue). The order is deterministic and computed at
   compile time, so `--list` shows it.
2. Within a suite, tests run in the order they are declared in it (kovarex, 2026-11-06). A
   suite is one block in one file, so that order is unambiguous; Factorio's sort (line, then name,
   then file) gives the same order. A generator's tests take the generator's place, in the order
   it adds them (Factorio's run first in their suite: line 0).
3. Then the tests outside any suite, in today's order.

`--shuffle [SEED]` replaces `--randomize-tests-order`. It keeps the dependency order between
suites and prints the seed.

**After a failure: Factorio's behaviour is the default.** What Factorio does (checked 2026-11-06):
- By default the run never stops early. A failed check ends only its test
  (`TestResults.cpp:33` throws `TerminateTestException`), and the runner goes on to the next test
  (`tests/TestRunner.cpp:33-60`), dependents of a failed suite included. The exit code is the
  number of failures (`TestRunner::Finish`, `libraries/UnitTest++/TestRunner.cpp:24-35`).
- `--exit-on-failure` (`tests/Main.cpp:179`, "Exit after a first failed test", off unless given)
  stops after the first test that failed. That test runs to the end, including its fixture
  teardown and `postTestValidation`; then `runList` returns false (`tests/TestRunner.cpp:52`),
  which also ends a `--repeat-count` loop (`tests/TestRun.cpp:204-208`).
- Together with the dependency order, this is the "stop fast" rule: the tests of A and B run
  before those of A+B, so with `--exit-on-failure` a failure in a dependency stops the run before
  any dependent suite starts. Without the flag, dependents run anyway. Nothing skips dependents
  on its own.
- `--fork` ignores it: `ForkLogic::runFork` gets no such flag and waits for every fork
  (`tests/ForkLogic.cpp:635-691`), and the fork command lines don't pass it on
  (`ForkTask.cpp:279-330`). Forks are built from cached test times, so suites in different forks
  run at the same time, whatever their dependencies.
- Interactive mode keeps a persistent list of failed tests and can re-run only those
  (`InteractiveTestSelect.cpp:42`, `:166`; `TestReporter.cpp:68`).
- Nothing in C:\Factorio's repository (justfile, scripts/, fastbuild/) passes `--exit-on-failure`;
  the CI command lines are not in the repository. kpp's `programs/factorio/tests.cmd` and
  `suites.py` don't pass it either.

kpp's runner follows this:
- **Default:** every selected test runs in dependency order, and a failure ends only its test.
- **`--exit-on-failure`:** stops after the first failed test, which finishes first, destructors
  and fixture teardown included. The rest count as not run (`N not run` in the summary, `"status":
  "not run"` in `--report`). Thanks to the order, a failure in a dependency stops the run before
  its dependents.
- **With `--jobs N`:** `--exit-on-failure` starts no further suite once a test has failed and lets
  the running ones finish. Factorio's `--fork` ignores the flag; kpp honours it, because the
  dependency order already lets it stop before dependents.
- **Reporting only, no change to what runs:** a failed test in a suite whose dependency (directly
  or further back) also failed is printed `failed: Beacon.X (after failures in: ElectricPole)`,
  and the summary lists the **root failures** (those without a failed dependency) first:
  `12 tests, 3 failed (1 root: ElectricPole.PoleConnects)`.
- `--stop-dependents` is dropped: Factorio has no such flag, and `--exit-on-failure` covers the
  case it was for.

**Filtering** (extends today's `--filter`):
- `--filter TEXT` matches the full name `Suite.Test` (or `file > test` outside a suite): `--filter Beacon.*`, `--filter *.Empty`.
- `--suite NAME` (repeatable) selects whole suites by name (case ignored on the command line).
- `--skip-suite NAME`.
- `--with-dependencies` adds every suite the selection depends on, as Factorio's
  `includeDependencies` does.
- `--from NAME` and `--after NAME` start at a test in the computed order.
- `--tag TAG`.
- `--exit-on-failure` (see above).
- Factorio's `--test`, `--contains`, `--suite-contains` and `--test-contains` are all
  `--filter` with a pattern.
- `-V` (invert) is left out unless someone needs it.
- A `--filter` no test matches, a `--suite` or `--skip-suite` no suite has, and a `--tag` no suite
  has are errors (exit code 2), not a run of nothing that passes.

**Reporting:** the same lines as today. `--report` adds `"suite"` and `"skippedBecause"` fields and a
per-test `"seconds"`. A test running longer than 5 minutes gets a warning line, as Factorio's
`SlowDetector` gives. XML: not needed, because `--report` is the machine format, and the game's
scripts (suites.py, failures.py; H's) would read it.

**Parallelism:** `--jobs N` (Q3 in design.md) schedules whole suites over N processes, the longest
first (`--durations FILE`, an earlier run's `--report`, else the most tests first). A suite does not
wait for its dependencies, as Factorio's `--fork` does not, because its results do not depend on
order, and with Q2's default kpp's do not either; the output is said in the suites' order, so the
root-failure report stays exact. With `--exit-on-failure` it waits, so that a failure in a dependency
stops the run before its dependents (2026-11-07: waiting always made the game's tests take their
longest chain, ManualBuilding to Simulations). A crash fails its test, and the process restarts for the rest of
its share. That replaces suites.py's process-per-suite and `--shards` by hand.

## (e) Converter mapping (C's)

| C++ | kpp |
|---|---|
| `SUITE(Name) { ... }` | `suite Name { ... }`. The suite's helper declarations stay inside it. The converter's expansion into `namespace SuiteName` and `currentSuite` goes away. |
| `DEPENDENCY(A);` (one or more, at the suite's top) | moved into the header: `suite Name depends A, B` |
| `SUITE_MODPACKS("vanilla")` | `tags vanilla` |
| `TEST_GENERATOR(Name) { ... addTest(suffix, f) ... }` (13) | `generate Name { ... addTest(suffix, f) ... }` |
| `TEST(Name)` | `test Name` |
| `TEST_FIXTURE(F, Name)` | `test Name : F` |
| `TEST_PARAMETRISED_FIXTURE(F, P, Name)` | `test Name : F(P)` |
| `CHECK(x)` | `require(x)`. `!!(x)` and the text argument are dropped: the compiler has the text. |
| `CHECK_EQUAL(a, e)` / `CHECK_UNEQUAL` | `require(a == e)` / `require(a != e)`. If both are `char` pointers: `require(StringView(a) == e)`. Integers: exact by the rule in (c). |
| `CHECK_BIGGER` / `_BIGGEREQUAL` / `_SMALLER` / `_SMALLEREQUAL(v, b)` | `require(v > b)`, `>=`, `<`, `<=` |
| `CHECK_NULL(p)` / `CHECK_NOT_NULL(p)` | `require(p == nullptr)` / `require(p != nullptr)` |
| `CHECK_CLOSE(a, e, t)` / `_NOT_CLOSE` / `_CLOSE_PERCENTAGE` | `require(isClose(a, e, t))` / `!isClose` / `isClosePercentage`. These are functions in a converted UnitTest++-replacement file (or the stdlib, if kovarex wants them there), and the arguments print by the rule in (c). |
| `CHECK_BETWEEN(v, l, u)` / `_BETWEENEQUAL` | `require(l < v && v < u)`. Text only: `&&` shows no values. Or `isBetween(v, l, u)` to show them. Recommendation: the function. |
| `CHECK_CONTAINS` / `_STARTS_WITH` / `_DOES_NOT_START_WITH` | `require(contains(a, s))` and so on, as functions |
| `CHECK_ARRAY_EQUAL(a, e)` | `require(a == e)` when both are containers. 5 uses: by hand where not. |
| `CHECK_*_SIGNED(..., id)` | the same with `, id` appended: `require(a == e, itemID)` |
| `CHECK_FAILED(msg << x)` | `fail(msg, x)`: the `<<` chain becomes arguments |
| `CHECK_THROW(expr, T)` | `requireThrows<T>(expr)` |
| `CHECK_THROW_EQUAL(expr, T, m)` | `require(requireThrows<T>(expr).what() == m)` |
| `CHECK_THROW_CONTAINS(expr, T, m)` | `require(contains(requireThrows<T>(expr).what(), m))` |
| `*_SOURCELOCATION(..., loc)` | drop `loc`; the helper becomes a `test` function |
| the tests' own `CHECK_*` macros (24) | `test` functions where the macro is function-like (`CHECK_LUA_NUMBER_EQUALS`, `CHECK_BUILDABILITY`). A macro that stringifies or declares stays expanded, but expanded into `require` and not `checkInternal`. |
| fixture and helper classes with checks (14 headers, 17 .cpp) | `test class` / `test` functions |
| 329 bare calls named `check(`, `require(` or `fail(` in tests/ (for example `CircuitNetworkHelper.hpp:26`) | the converter renames the function where a test body or `test` function calls it, since those names are statements there |

After the mapping, `libraries/UnitTest++` in factorio-in-kpp shrinks to what the game's own runner
still uses. That is TestGlobals, CRC mode and desync reporting, which `tests/TestRun.cpp` and
`MetaTestRunner.hpp` drive. Q5 (decided): the game runs under the native runner, with setup hooks.

## (f) Questions for kovarex (all decided 2026-11-06)

1. **Factorio's `CHECK` ends the test. Should the converter emit `require` for it, or `check`?**
   Decided (kovarex, 2026-11-06): kpp keeps both, `check` (continues) and `require` (ends the
   test), and the converter maps every Factorio `CHECK*` macro to `require`. Factorio's fork throws
   on every failure and its tests rely on that (`CHECK_NOT_NULL(p); p->...`); `check` would run
   code the C++ never ran. New kpp tests choose between `check` and `require`.
2. **What happens after a failure?** Decided (kovarex, 2026-11-06), as recommended: what Factorio does. By default everything runs
   and a failure ends only its test (`TestResults.cpp:33`, `tests/TestRunner.cpp:33-60`).
   `--exit-on-failure` (`tests/Main.cpp:179`) stops after the first failed test, so with
   dependency order a failure in a dependency stops the run before its dependents. kpp adds two
   things: it honours the flag under `--jobs` too (Factorio's `--fork` ignores it), and it marks
   failures that come after a failed dependency in the report, without changing what runs. The
   9,501-test comparison with the C++ build stays one to one.
3. **Dependencies between suites only, or between single tests too?** Decided (kovarex, 2026-11-06), as recommended: suites only.
   Factorio's 69 are all suite-level, and test-level `depends` can be added when something needs it.
4. **Should integers in a check compare exactly across signedness?** Decided (kovarex, 2026-11-06), as recommended: yes, inside
   `check` and `require` only. It keeps `CHECK_EQUAL`'s meaning. The alternative is a converter
   rewrite to `cmpEqual(a, b)` at each mixed-sign site, which loses the operand display.
5. **Should the converted game run under kpp's generated runner, or keep its own `main` with the
   native tests registered?** Decided (kovarex, 2026-11-06), as recommended: the native runner, with a hook for the game's setup.
   The runner would call `testsSetup(args)` and `testsTeardown()` if the program defines them,
   and pass the arguments it does not know to them. The game's `--config`, `--crc-mode`, heavy
   mode and graphics options then stay the game's, while suites, filters, order and `--jobs`
   are the runner's. TestRun.cpp's per-test validation (`postTestValidation`, leaked entities,
   `appManager->eraseIf`) needs a per-test hook too: `testAfterEach()`.
6. **Suite and test names: strings or identifiers?** Changed after kovarex's question
   (2026-11-06, "why not just suite DragBuildingBelt?"): suite names are identifiers in their own
   namespace (`suite DragBuildingBelt depends DragBuilding, TransportBelt`). Decided (kovarex,
   2026-11-06): suite names are a separate name context, and test names are identifiers too,
   with no string form.
7. **Duplicate test names: unique within a suite, or across the program as Factorio requires?**
   Decided (kovarex, 2026-11-06): test names are scoped per suite, so two suites may each have a
   test called `Empty`; two tests of one name in one suite are a compile error. Reports and
   `--filter` use `Suite.Test` as the full name. Factorio's names are unique program-wide today,
   so the converted game is unaffected; its `--test NAME` becomes `--filter *.NAME`.
8. **`requireThrows<T>(expr)` and `fail(...)` as statements of the language, or stdlib functions
   with a lazily evaluated argument?** Decided (kovarex, 2026-11-06), as recommended: statements, like `check`. kpp has no lazy
   arguments, and a lambda wrapper would bring back the noise this proposal removes.
9. *Superseded* (kovarex, 2026-11-06): the question was whether two names differing only in case
   are an error. Names are case-sensitive as everywhere in kpp, so they are simply two names; only
   the runner's command-line matching ignores case.
10. **Is `--exit-on-failure` on by default for local runs?** Decided (kovarex, 2026-11-06), as
    recommended: no, off by default, as in Factorio.

## Migration: quoted test names in C:\kpp

Counted in C:\kpp master e8caa8db (`git grep`, tracked files): **98 `test "..."` declarations**
would be renamed to identifiers. **68 of them are not identifiers today** (`square of three`,
`parse a number`, `bump counts`, `Direction::isVertical`, a name with quotes in it); the other 30
only lose their quotes.

| Where | Declarations |
|---|---|
| compiler/tests/spec/kpp (10 snippets: test_runner, test_check_values, test_require, test_exception, test_filter, test_list, test_report, test_in_class, test_build_error, tests_in_compiler) | 22 |
| libraries/*/tests (json 13, markdown 10 + 3 in conformance/CommonMarkTests.kpp.in, mariadb 7, url 6, sha256 6, html 6, http 5, bcrypt 5, net 3, httpclient 3, base64 3) | 70 |
| kppnet/site/src/SiteTests.kpp | 3 |
| docs/design.md examples (8.6 at line 417, "How to use it" at line 2585) | 2 |
| readme.md:177 | 1 |
| stdlib/, programs/, compiler/tests/run | 0 |

Besides the declarations, 13 prose or comment lines describe the syntax as `test "name"` and change
with it: design.md 225, 423, 2562, 2616, 2679, 2741; ide.md 285; macro-survey.md 82;
libraries/README.md 10; parser.cpp 3475; tests.hpp 6; and the two spec headers (test_runner,
tests_in_compiler), whose text docs/spec-status.md regenerates. The spec snippets' `.expected`
files change where they print a renamed test (`failed: file > name`, the `--list` and `--report`
output). test_report's `a "quoted" name` test exists to check JSON escaping of the name; with
identifier names the escaping matters only for file paths, and the snippet needs a new case.

The rename is one commit together with the parser change, by whoever implements it (A's parser,
the libraries' owners told through the coordinator), so no tree ever has both forms.

## Next steps
- The rule is design.md 8.7, with its spec snippets (pending until implemented).
- The parser and the runner, then C's converter mapping (section e). That last step needs a whole
  `convert.cmd` and the game's tests, 9,501, compared by test name with the C++ build.

## The conversion as it stands (2026-11-06)

`kppc --convert-native-tests` (Converter/NativeTests.inl), convert.cmd's default: the 9,501 tests of Factorio
4c178a5b915 pass under the native runner, and their names are the C++ build's (its `--xml` against the
runner's `--report`), with three mappings: a generated test is `Suite.Generator.name` where UnitTest++ has
`Suite.Generator_name`, and 2x2UndergroundPipeSmartFixing and 4GBZipFileWriteRead, whose names are no
identifiers, are UndergroundPipe2x2SmartFixing and ZipFile4GBWriteRead (overrides/source-edits.txt).
- libraries/UnitTest++ stays a library: TestTestSelector, TestFailedTests, TestInteractiveTestSelect and
  others test the game's selection and fork code on its Test and Suite classes. Nothing runs through it.
- `--fork`, `--interactive` and the selection options (`--test`, `--tests`, `--suites`, ...) are gone with
  tests/Main.cpp's run: `--suite`, `--filter` and suites.py's shards do that. `--save` and `--add-tags` stay.
- UnitTest++'s CheckStartsWith answers true for a container that does not start with the prefix
  (Checks.h:290, after writing its failure). `UnitTest::startsWith` answers as it does, as
  Deconstruction.DragAndDropDeconstructionPlanner{Entity,Tile}FilterWithLatency fail otherwise.
