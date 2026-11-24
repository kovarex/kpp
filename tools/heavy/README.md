# heavy: kpp's way to the heavy-job lock

kpp's scripts that load the whole machine run through `tools\heavy\heavy.cmd`: compiler\tests\gate.cmd,
run_tests.cmd and spec.cmd, compiler\tests\differential\run.py, and convert.cmd, tests.cmd, run.cmd,
link.cmd, harness.cmd, format-check.cmd, c-libraries.cmd and lua-trial.cmd under programs\factorio.

    tools\heavy\heavy.cmd [--exclusive] [--name TEXT] [--estimate T] [--local or --travel] COMMAND LINE...

The lock itself is not part of kpp (kovarex, 2026-11-24): it is heavy-queue
(github.com/kovarex/heavy-queue), which a machine that has it calls at `C:\kpp-sandbox\heavy.cmd`, and
heavy.cmd hands it the command line as it came. One heavy job at a time per machine, time estimates, jobs
that run on another machine, and the board: heavy-queue's README and lock\README.md.

Without it (CI, another machine), heavy.cmd drops the lock's options and runs the command at once, with
KPP_HEAVY_HELD set, as the lock sets it for its jobs: a script the command calls that would come through
heavy.cmd again runs at once.
