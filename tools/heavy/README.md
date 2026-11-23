# heavy: one heavy job at a time on PRACOVNA

kovarex, 2026-11-23: every thread benchmarks, builds and tests one at a time.
Gates, run_tests, spec, convert, the game's builds, links and test runs, the differential suite, and
every benchmark or timing run hold the machine's one heavy slot.

    tools\heavy\heavy.cmd [--exclusive] [--name TEXT] COMMAND LINE...
    tools\heavy\heavy.cmd --status

Outside a worktree, `C:\kpp-sandbox\heavy.cmd` does the same thing. It calls C:\kpp's copy.

- **Scripts that take the slot themselves:** compiler\tests\gate.cmd, run_tests.cmd, spec.cmd, and
  compiler\tests\differential\run.py. Under programs\factorio: convert.cmd, tests.cmd, run.cmd,
  link.cmd, harness.cmd, format-check.cmd, c-libraries.cmd and lua-trial.cmd. The shared benchmark
  script, C:\kpp-sandbox\scratch\bench\run.py, takes it with --exclusive.
- **Running anything else heavy:** prefix it with `heavy.cmd`. That includes direct game builds and
  test runs, such as `heavy.cmd kpp factorio release` and
  `heavy.cmd kpp factorio-test debug --config ...`.
- **Benchmarks and timing runs:** use `heavy.cmd --exclusive COMMAND...`. The job takes the same slot,
  then waits until total CPU stays under 15% for 20 seconds, and holds quiet-window.flag while it
  runs. Never time anything outside --exclusive.
- **Waiting:** waiters queue in arrival order. A waiter prints one line to stderr naming the holder,
  then a new line only when the reason changes. Nothing needs polling, because the command starts
  once the slot is free.
- **Holder dies:** the slot is a named mutex (Global\kpp-heavy), so Windows frees it when the
  holding process dies. A killed job never leaves the machine locked. A dead waiter's queue file is
  dropped.
- **Nested scripts:** the command runs with KPP_HEAVY_HELD set. Scripts it calls, such as gate.cmd
  calling run_tests.cmd and spec.cmd, run at once instead of waiting for their own parent.
- **schedule.md:** the coordinator's `HOLD: yes` in C:\kpp-sandbox\scratch\schedule.md line 1 also
  makes every job wait.
- **CI and other machines:** CI (GITHUB_ACTIONS) and machines without C:\kpp-sandbox run the command
  straight away.
