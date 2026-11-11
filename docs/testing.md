# Testing kpp code

A short guide to kpp's tests: how to write them, run them, run them in parallel, and give a program's
tests their own setup and options. The rules themselves are in
[design.md](design.md) 8.6 and 8.7
and the runner's details in design.md's "Running and testing kpp code".
The runner is kpp code inside the compiler (`compiler/src/tests.cpp`), the same for every program:
a program does not write its own.

## Writing tests

```
int square(int x) { return x * x; }

test squareOfThree            // a test outside a suite: its full name is `square.kpp > squareOfThree`
{
  check(square(3) == 8);      // fails and goes on: square.kpp:5:3: check failed: square(3) == 8 (9 == 8)
  require(square(0) == 0);    // a failed require ends the test; a failed check goes on
}

suite Squares                 // a suite: its tests' full names are Squares.Test
{
  int twice(int x) { return 2 * x; }   // a helper, seen by this suite's tests only

  test Positive { check(square(2) == 4, "with ", 2); }   // arguments after the condition: a message
  test Negative { check(square(-2) == 4); }               // tests run in the order they are written
}
```

- Names are identifiers. A test's name is scoped to its suite; a suite's name hides nothing (a
  `suite Beacon` beside a `class Beacon` is fine).
- `fail(arguments...)` fails and ends the test. `requireThrows<T>(expr)` is the `T` that `expr`
  throws, and fails and ends the test when it throws none; `checkThrows<T>(expr)` is an `Optional<T>`
  and goes on.
- A failed check shows its operands' values (`(9 == 8)`), a call's arguments' values, and its
  message. Inside a check, signed and unsigned integers compare by value, and `==` of two char
  pointers is an error (it would compare addresses: write `StringView(a) == b`).
- A function or class declared `test` (and every function and class in a suite) is test code: it
  may use `check`, `require` and `fail`, and a failure in it says where it was called from.

### Fixtures

```
class Counter
{
public:
  Counter(int start = 0) : value(start) {}
protected:
  int value;
};

suite Counting
{
  test FromTen : Counter(10)  // the body is a member of a class derived from Counter
  {
    ++this.value;             // its members through this. (design.md 23c)
    require(this.value == 11);
  }
}
```

The fixture is built before the body and destroyed after it, also when the body fails.

### Dependencies

```
suite Vector { test PushBack { ... } }
suite Inventory depends Vector { test Insert { ... } }   // runs after every test of Vector
```

A suite runs after the suites it depends on (an unknown name or a cycle is a compile error). When
something basic breaks, the simplest failing test comes first: a failure in a suite after a failed
dependency is marked `failed: Inventory.Insert (after failures in: Vector)`, and the summary lists
the root failures, those no failed dependency explains, the most fundamental first:

```
2 tests, 2 failed (root failures: Vector.PushBack; 1 after failures in their dependencies)
```

`generate Name { addTest(name, function); }` in a suite adds tests when the runner starts (one per
save file, say); `tags t` on a suite runs it only under `--tag t`.

## Running tests

```
kpp --test square.kpp                       # builds the tests (and not main) and runs them
kpp --test src -- --filter Squares.*        # runner arguments after --
kpp --test                                  # in a project: its tests (build.kpp's test roots)
```

The runner's options: `--filter TEXT` (the full name, `*` matching anything), `--suite NAME`,
`--skip-suite NAME`, `--with-dependencies`, `--from NAME`, `--after NAME`, `--tag TAG`,
`--exit-on-failure`, `--shuffle [SEED]`, `--slow SECONDS`, `--jobs N`, `--durations FILE`, `--list`,
`--report FILE` (`-` for stdout: one JSON line per test, then a summary line) and `--help`. The exit
code is 0 when every test passed, 1 when one failed, 2 for a bad argument.

## Interactive mode: `-i`

`-i` (or `--interactive`) runs the program's `testsSetup` and generators once, then shows a menu, and
a key chooses what runs (kovarex, 2026-11-12, as Factorio's own test program had it, so that a run's
selection is not typed on the command line each time):

- **space**: the last choice again (kept between runs);
- **a**: all the tests, in parallel: worker processes as `--jobs` starts them, as many as Factorio's
  test program used (the hardware threads, 1 to 64, then the program's `testsWorkers(count)`);
- **A**: all the tests, in this process;
- **t** / **s**: a test or a suite, found by typing part of its name: each key shows how many match
  and the highlighted one; up and down move it, Enter takes it, Escape goes back;
- **v**: the test whose name is on the clipboard (a filter when it is no test's whole name);
- **f**: the tests that failed in the last run (kept between runs);
- **q**: quit (`testsTeardown` runs then);
- **T**, **S**, **V**, **F**: the same, with the suites they depend on.

The chosen tests run in the same process, so a debugger's breakpoints hold: in Visual Studio, put
`-i` in the project's Command Arguments and press F5. The menu then shows again. The last choice
and the failed tests are kept in `<executable>.interactive` beside the executable. On a console the
runner reads one key at a time; from a pipe or a file (a script, `compiler/tests/interactive.ps1`)
it reads bytes, Enter being `\r`. On Linux and FreeBSD a key counts once Enter is pressed, and `v`
has no clipboard yet.

A program adds keys of its own with two functions: `std::String testsInteractiveKeys()`, the text
added to the menu's last line, and `bool testsInteractiveKey(int key)`, true when the key was its
(the game's `h` turns its heavy mode on and off). For `a`'s workers, `testsWorkerArguments()` adds
arguments to their command line (the game's `--heavy-mode`) and `testsWorkers(count)` sets how many
start (heavy mode's fewer, as Factorio's). `--jobs` does not apply in this mode.

## Parallel runs: `--jobs N`

`--jobs N` runs the tests in N worker processes (processes, not threads: tests may use globals).

- The workers start once, at the beginning, and each runs the program's `testsSetup` itself.
- The first process runs no test: it hands out whole suites, the most tests first, or with
  `--durations FILE` (an earlier run's `--report`) single tests, the longest first, so that a long
  suite spreads over the workers.
- What each test prints is collected and printed in the suites' dependency order, so the output,
  the `after failures in` marks and the root failures are the same as a run in one process.
- A worker that crashes fails the test it was in; another takes the rest of its suite.
- A suite does not wait for its dependencies (the processes share nothing), except under
  `--exit-on-failure`, where suites stay whole and wait, so that a failure in a dependency stops the
  run before its dependents.

## A program's own setup and options: the hooks

The runner takes its own options and passes every other argument to the program. A program
customises its tests by defining these functions; each one it does not define does nothing.

| Hook | When the runner calls it |
|---|---|
| `void testsSetup(const Vector<String>& arguments)` | once, before the first test, with the arguments the runner does not know (without it, an unknown argument is an error) |
| `void testsHelp()` | after the runner's own `--help` text: the program's options |
| `void testBeforeEach()` | before each test; a failure in it leaves the test's body out |
| `void testRun(Function<void()> run)` | around each test's body; it may call `run` again |
| `void testAfterEach()` | after each test; a failure in it fails that test |
| `void testsTeardown()` | after the last test |
| `void testAssertFailed(const char* file, int line, int column, const char* text)` | instead of what a failing `assert` does in a test |

A test build also gives the program `currentTestName()` (the running test's full name),
`testWorker()` (which `--jobs` worker this is, from 1; 0 without `--jobs`), `runTest(name)` and
`runCaptured(function)`.

### The game, as a worked example

factorio-in-kpp's `tests/TestHooks.kpp` turns Factorio's own test program into hooks:

- `testsSetup` gives each `--jobs` worker its own `--write-data DIR/worker-N`, then passes the
  arguments to the game's `fmain`, whose own option parser takes `--config`, `--crc-mode`,
  `--crc-file`, heavy mode, graphics and the rest, as Factorio's test program did.
- `testsHelp` prints that parser's help.
- `testBeforeEach` opens the CRC dumper under the test's UnitTest++ name, so the CRC files match the
  C++ build's.
- `testRun` catches a latency test's desync and runs the test again to write the desync report.
- `testAfterEach` is Factorio's post-test validation (leak checks) and its cleanup of app states.
- `testsTeardown` writes the CRC file and checks for leaked entities.

`programs/factorio/tests.cmd --shards N` runs the game's tests with `--jobs N`, the last passing
run's report as `--durations`.
