# Time: Duration, TimePoint, SteadyClock, SystemClock

What std::chrono converts to: `std::chrono::milliseconds` is `Milliseconds`, `steady_clock` is
`SteadyClock`, `duration_cast` is `durationCast`, and `10ms` is `Milliseconds(10)`.

## Ratio

`Ratio<N, D>` is the fraction N/D, known at compile time: members `num` and `den`, the fraction in
lowest terms with a positive denominator (`Ratio<2, -4>` has num -1 and den 2). `Nano`, `Micro`,
`Milli`, `Centi`, `Deci`, `Kilo`, `Mega` are the usual ones.

Ratios combine at compile time: `RatioAdd<R1, R2>`, `RatioSubtract`, `RatioMultiply`,
`RatioDivide` (each a Ratio in lowest terms, given whenever it fits: common factors are divided out
before the products are formed, as std's) and `ratioEqual<R1, R2>`, `ratioNotEqual`,
`ratioLess`, `ratioLessEqual`, `ratioGreater`, `ratioGreaterEqual` (std::ratio_add ... ratio_greater_equal_v).

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
  To (an integer count computed as std's, in a u64 when either rep is a 64-bit unsigned type and in
  an i64 otherwise); `floor<To>(d)` and `ceil<To>(d)` round down and up instead, and `round<To>(d)` (an integer
  To) to the nearest, a tie to the even count; all three exact for any count (compared in the
  common duration).
- The *common duration* of two durations is the one both convert to without loss: its rep is the
  type of the sum of the two reps, its period the greatest period that divides both, so the finer
  one when the other is a whole multiple of it (Milliseconds and Seconds: Milliseconds; this is
  so for any two of the standard durations), else gcd of the numerators over lcm of the
  denominators (`Ratio<1, 2>` and `Ratio<1, 3>`: `Ratio<1, 6>`).
- `+d`, `-d`; `d1 + d2`, `d1 - d2` and `d1 % d2` for any two durations are in their common
  duration (`Seconds(2) + Milliseconds(1500)` is `Milliseconds(3500)`), and `d1 / d2` is the
  number of times d2 fits in d1, a count of the common rep; `d * n`, `n * d`, `d / n` and `d % n`
  for an integer n keep the type, `d * x`, `x * d` and `d / x` for a floating-point x give
  `Duration<double, Period>`. `+=`, `-=`, `*=`, `/=`, `++`, `--` change the count (`d++` and `d--` return the old duration).
- `==`, `!=`, `<`, `<=`, `>`, `>=` compare durations of any two types in their common duration,
  by the time they stand for (`Minutes(1) == Seconds(60)`).
- `abs(d)`: d without its sign (a signed or floating-point rep). `treatAsFloatingPoint<Rep>`:
  whether Rep is floating point (std::chrono::treat_as_floating_point_v).
- `Weeks` (7 days), `Years` (365.2425 days, Ratio<31556952>) and `Months` (a twelfth of that,
  Ratio<2629746>), as std's weeks, years and months.
- `HoursMinutesSeconds<D>(d)` (std::chrono::hh_mm_ss): d's absolute value taken apart:
  `hours()`, `minutes()`, `seconds()`, `subseconds()` (what is below a second), `is_negative()`,
  `to_duration()` (d again). Its `precision`, the duration subseconds() and to_duration() give, is
  std's: seconds when D's period is whole seconds, otherwise 10^-n seconds for the fewest n (up to 18)
  that holds D's period exactly, or 6 digits (microseconds) when none does (a third of a second);
  `fractional_width` is n.

## TimePoint<Clock, Dur>

A point in time of a clock: the Duration since the clock's epoch, of type Dur (`Clock::duration`
unless given).

- `TimePoint()`: the epoch; `TimePoint(d)` (explicit); from a time point of the same clock whose
  duration converts implicitly. `time_since_epoch()`; `min()`, `max()`.
- `t + d`, `d + t` and `t - d` for any duration d: a time point of the clock in the common duration
  of Dur and d's (a `TimePoint<SystemClock, Seconds>` plus Milliseconds counts in milliseconds);
  `t += d`, `t -= d` (d converted to Dur); `t1 - t2` for two time points of the clock: the common
  duration of theirs between them; `==`, `!=`, `<`, `<=`, `>`, `>=` between time points of the
  clock in any durations.
- `timePointCast<To>(t)`: t with its duration cast to To.

## Clocks

Each clock has the members `rep`, `period`, `duration`, `time_point`, `is_steady` and `now()`.

- `SteadyClock`: never goes back; nanoseconds since an unspecified start (the machine's boot),
  read from the performance counter. `HighResolutionClock` is the same clock.
- `SystemClock`: the wall clock, UTC, in ticks of 100 ns since 1970-01-01; it jumps when the system
  time is set. `to_time_t(t)`: whole seconds since 1970, rounded down; `from_time_t(s)`: back.
- `FileClock` (std::chrono::file_clock): the file system's clock, a file's last write time
  (`filesystem::file_time_type`), in ticks of 100 ns since 1601-01-01 UTC (Windows' FILETIME) on
  every target. `to_sys(t)` and `from_sys(t)`: the same instant as a SystemClock time point and
  back. *Difference from C++*: the epoch is implementation-defined there (MSVC's is 1601, as
  here; libstdc++'s another), and MSVC has no to_sys (clock_cast).

## Reading a time from text

`parseTime(text, format, &tm)` (what std::get_time does with a stream): reads the text by a
strftime-style format into the fields of a `tm`: `%Y`, `%m`, `%d`, `%H`, `%M`, `%S` and `%%`;
plain characters must match, a space matches any run of white space. It returns whether the whole
format matched; the fields read before a mismatch, or a specifier it does not know, keep their
values.

## Calendar time

UTC only, proleptic Gregorian (year 0 is 1 BC), no time zones and no leap seconds: what web
timestamps need. `stdlib/core/Calendar.kpp`; the dates are Howard Hinnant's `days_from_civil` and
`civil_from_days`.

- `CivilTime`: `year`, `month` (1 to 12), `day`, `hour`, `minute`, `second` (0 to 59),
  `nanosecond`, `weekday` (0 Sunday to 6 Saturday).
- `toCivilTime(t)`: a SystemClock time point's fields (the nanoseconds a multiple of 100).
  `fromCivilTime(c)`: the time point, an `Optional` that is empty when a field is out of its range
  (February 30, hour 24, second 60), the year outside -29000 to 29000, or the time outside what
  SystemClock's 100 ns ticks hold (they reach back into the year -27258); `weekday` is not read.
- `daysFromCivil(year, month, day)`: days since 1970-01-01, negative before it.
  `isLeapYear(year)`, `daysInMonth(year, month)`.
- `formatRfc3339(t, fractionDigits = 0)`: `2026-11-01T10:31:07Z`, with 0 to 7 digits of the
  second, truncated (`.123Z`); always `Z`. `formatHttpDate(t)`: RFC 9110's IMF-fixdate,
  `Sun, 06 Nov 1994 08:49:37 GMT`. Both throw `OutOfRange` for a year outside 0 to 9999, which
  has no four-digit form, and `formatRfc3339` throws `InvalidArgument` for another number of
  digits.
- `parseRfc3339(text)`: RFC 3339's date-time, `YYYY-MM-DDTHH:MM:SS`, an optional fraction of any
  length (digits past the seventh are dropped: the clock counts 100 ns), then `Z` or `+HH:MM` /
  `-HH:MM` (`-00:00` is UTC); `t` and `z` may be lowercase. An `Optional`, empty for anything
  else: **a leap second (`:60`) is rejected**, since SystemClock has none; so are a space for
  `T`, a missing offset, an offset hour past 23, two-digit years, a date the calendar lacks
  (`2023-02-29`) and text after the timestamp.
- `parseHttpDate(text)` and `parseHttpDate(text, now)`: any of RFC 9110 5.6.7's three forms,
  which a recipient must read: IMF-fixdate, RFC 850 (`Sunday, 06-Nov-94 08:49:37 GMT`) and
  asctime (`Sun Nov  6 08:49:37 1994`). Case-sensitive, as the RFC's grammar is. RFC 850's
  two-digit year is in now's century unless that is more than 50 years after now, then in the
  century before (the RFC's rule; `now` is `SystemClock::now()` unless given). Empty for anything
  else, for a weekday that is not the date's, and for a leap second.

Tested by spec library/time_calendar (RFC 3339 5.8's examples, RFC 9110's three forms, every
rejection above, every day of the years 1 to 9999 there and back) and library/time_python_datetime
(600 random instants against Python's datetime).

Waiting functions take any duration: `thisThread::sleepFor(d)`, a Future's `waitFor(d)`, a
ConditionVariable's `waitFor(lock, d)`; they wait at least d, rounded up to whole milliseconds.

*Differences from C++*: the common rep of two reps is the type of their sum, which is
std::common_type's except for two reps of one type narrower than int (two `short` reps give an
`int` count); `d * n` and `d / n` keep the rep for an integer n where C++ takes the common type of
the rep and n's type; the calendar is CivilTime and the functions above instead of C++20's
`year_month_day` family and `std::format` of time points; no `utc_clock` or time zones (FileClock
is `file_clock`).
