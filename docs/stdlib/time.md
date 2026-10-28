# Time: Duration, TimePoint, SteadyClock, SystemClock

What std::chrono converts to: `std::chrono::milliseconds` is `Milliseconds`, `steady_clock` is
`SteadyClock`, `duration_cast` is `durationCast`, and `10ms` is `Milliseconds(10)`.

## Ratio

`Ratio<N, D>` is the fraction N/D, known at compile time: members `num` and `den`. `Nano`, `Micro`,
`Milli`, `Centi`, `Deci`, `Kilo`, `Mega` are the usual ones.

## Duration<Rep, Period>

A count of ticks of type Rep (an integer or floating-point type), each tick Period seconds long (a
Ratio, `Ratio<1>` unless given). The usual durations count in `long long`: `Nanoseconds`,
`Microseconds`, `Milliseconds`, `Seconds`, `Minutes` (60 s), `Hours`, `Days`.

- `Duration()`: zero; `Duration(count)` (explicit): that many ticks. `count()`: the ticks.
  `zero()`, `min()`, `max()`: the durations of 0 ticks and of the least and greatest Rep.
- Conversion from another duration is implicit when nothing is lost: when this Rep is floating
  point, or the other Rep is an integer and the other period is a whole multiple of this one
  (`Nanoseconds ns = Milliseconds(5);`, `Duration<double> s = Milliseconds(1500);` is 1.5).
  Other conversions go through `durationCast<To>(d)`, which truncates toward zero for an integer
  To; `floor<To>(d)` and `ceil<To>(d)` round down and up instead.
- `+d`, `-d`; `d1 + d2` and `d1 - d2` for two durations of the same type; `d * n`, `d / n` and
  `d % n` for an integer n keep the type, `d * x` and `d / x` for a floating-point x give
  `Duration<double, Period>`; `d1 / d2` of the same type is the number of times d2 fits in d1
  (Rep), `d1 % d2` the rest. `+=`, `-=`, `*=`, `/=`, `++`, `--` change the count.
- `==`, `!=`, `<`, `<=`, `>`, `>=` compare durations of any two types by the time they stand for
  (`Minutes(1) == Seconds(60)`).

## TimePoint<Clock, Dur>

A point in time of a clock: the Duration since the clock's epoch, of type Dur (`Clock::duration`
unless given).

- `TimePoint()`: the epoch; `TimePoint(d)` (explicit); from a time point of the same clock whose
  duration converts implicitly. `time_since_epoch()`; `min()`, `max()`.
- `t + d`, `t - d` (any duration, converted to Dur), `t += d`, `t -= d`; `t1 - t2`: the Dur between
  them; `==`, `!=`, `<`, `<=`, `>`, `>=` between time points of the same type.
- `timePointCast<To>(t)`: t with its duration cast to To.

## Clocks

Each clock has the members `rep`, `period`, `duration`, `time_point`, `is_steady` and `now()`.

- `SteadyClock`: never goes back; nanoseconds since an unspecified start (the machine's boot),
  read from the performance counter. `HighResolutionClock` is the same clock.
- `SystemClock`: the wall clock, UTC, in ticks of 100 ns since 1970-01-01; it jumps when the system
  time is set. `to_time_t(t)`: whole seconds since 1970, rounded down; `from_time_t(s)`: back.

Waiting functions take any duration: `thisThread::sleepFor(d)`, a Future's `waitFor(d)`, a
ConditionVariable's `waitFor(lock, d)`; they wait at least d, rounded up to whole milliseconds.

*Differences from C++*: `d1 + d2`, `d1 - d2`, `d1 / d2` and `d1 % d2` need durations of the same
type (C++ gives the common type of any two; convert one first, the finer type takes the coarser
without loss); a number times a duration is written `d * n`; the common period of two durations is
the finer one, where C++ takes the greatest common divisor of the two (the same for every pair of
the standard durations); there are no calendar types, no `file_clock`, `utc_clock` or time zones,
and no `round` or `abs` of durations.
