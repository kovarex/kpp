#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <system_error>
namespace filesystem = std::filesystem;
using ErrorCode = std::error_code;
using String = std::string;
template <typename T> using Vector = std::vector<T>;
void show(const char* label, const std::string& s) { printf("%s: [%s]\n", label, s.c_str()); }
void flag(const char* label, bool b) { printf("%s: %s\n", label, b ? "true" : "false"); }
void number(const char* label, long long n) { printf("%s: %lld\n", label, n); }
template <typename V> void sortStrings(V& v) { std::sort(v.begin(), v.end()); }
void appendText(std::string& to, const std::string& s) { to += s; }
// kpp's chrono names on std::chrono's, for the C++ side of the chrono batteries.
// kpp's time names on std::chrono's (C++20's calendar and std::format for the calendar's
// functions), for the C++ side of tm1.
#include <chrono>
#include <ratio>
#include <format>
#include <optional>
#include <stdexcept>
template <long long N, long long D = 1> using Ratio = std::ratio<N, D>;
template <typename R, typename P = std::ratio<1>> using Duration = std::chrono::duration<R, P>;
using Nanoseconds = std::chrono::nanoseconds;
using Microseconds = std::chrono::microseconds;
using Milliseconds = std::chrono::milliseconds;
using Seconds = std::chrono::seconds;
using Minutes = std::chrono::minutes;
using Hours = std::chrono::hours;
using Days = std::chrono::duration<long long, std::ratio<86400>>;
using Weeks = std::chrono::duration<long long, std::ratio<604800>>;
using Years = std::chrono::duration<long long, std::ratio<31556952>>;
using Months = std::chrono::duration<long long, std::ratio<2629746>>;
using SystemClock = std::chrono::system_clock;
template <typename C, typename D = typename C::duration> using TimePoint = std::chrono::time_point<C, D>;
template <typename To, typename R, typename P> To durationCast(const std::chrono::duration<R, P>& d) { return std::chrono::duration_cast<To>(d); }
using std::chrono::floor;
using std::chrono::ceil;
using std::chrono::round;
using std::chrono::abs;
template <typename A, typename B> using RatioAdd = std::ratio_add<A, B>;
template <typename A, typename B> using RatioSubtract = std::ratio_subtract<A, B>;
template <typename A, typename B> using RatioMultiply = std::ratio_multiply<A, B>;
template <typename A, typename B> using RatioDivide = std::ratio_divide<A, B>;
template <typename A, typename B> constexpr bool ratioEqual = std::ratio_equal_v<A, B>;
template <typename A, typename B> constexpr bool ratioLess = std::ratio_less_v<A, B>;
template <typename A, typename B> constexpr bool ratioLessEqual = std::ratio_less_equal_v<A, B>;
template <typename A, typename B> constexpr bool ratioGreater = std::ratio_greater_v<A, B>;
template <typename R> constexpr bool treatAsFloatingPoint = std::chrono::treat_as_floating_point_v<R>;
template <typename D> using HoursMinutesSeconds = std::chrono::hh_mm_ss<D>;
using OutOfRange = std::out_of_range;
using InvalidArgument = std::invalid_argument;
template <typename T> using Optional = std::optional<T>;

// The calendar (time.md, "Calendar time") from C++20's: year_month_day, weekday, hh_mm_ss.
struct CivilTime
{
  int year = 1970, month = 1, day = 1, hour = 0, minute = 0, second = 0, nanosecond = 0, weekday = 4;
};
using CivilTicks = std::chrono::duration<long long, std::ratio<1, 10000000>>;
using CivilPoint = std::chrono::time_point<std::chrono::system_clock, CivilTicks>;
template <typename D> CivilTime toCivilTime(const std::chrono::time_point<std::chrono::system_clock, D>& t)
{
  using namespace std::chrono;
  auto dp = floor<days>(t);
  year_month_day ymd{dp};
  hh_mm_ss<CivilTicks> hms{duration_cast<CivilTicks>(t - dp)};
  CivilTime c;
  c.year = int(ymd.year());
  c.month = int(unsigned(ymd.month()));
  c.day = int(unsigned(ymd.day()));
  c.hour = int(hms.hours().count());
  c.minute = int(hms.minutes().count());
  c.second = int(hms.seconds().count());
  c.nanosecond = int(hms.subseconds().count() * 100);
  c.weekday = int(weekday{dp}.c_encoding());
  return c;
}
// (a year SystemClock cannot hold in 100 ns ticks is printed as such by the battery: tm1Unheld)
bool tm1Unheld = false;
Optional<CivilPoint> fromCivilTime(const CivilTime& c)
{
  using namespace std::chrono;
  tm1Unheld = false;
  if (c.year < -29000 || c.year > 29000 || c.month < 1 || c.month > 12 || c.day < 1 || c.hour < 0 || c.hour > 23 || c.minute < 0 || c.minute > 59 ||
      c.second < 0 || c.second > 59 || c.nanosecond < 0 || c.nanosecond > 999999999)
    return {};
  year_month_day ymd{year{c.year}, month{unsigned(c.month)}, day{unsigned(c.day)}};
  if (!ymd.ok())
    return {};
  __int128 seconds = (__int128)sys_days{ymd}.time_since_epoch().count() * 86400 + c.hour * 3600 + c.minute * 60 + c.second;
  __int128 ticks = seconds * 10000000 + c.nanosecond / 100;
  if (ticks > (__int128)INT64_MAX || ticks < (__int128)INT64_MIN)
  {
    tm1Unheld = true;
    return {};
  }
  return CivilPoint(CivilTicks((long long)ticks));
}
long long daysFromCivil(long long y, int m, int d)
{
  using namespace std::chrono;
  return sys_days{year_month_day{year{int(y)}, month{unsigned(m)}, day{unsigned(d)}}}.time_since_epoch().count();
}
bool isLeapYear(long long y) { return std::chrono::year{int(y)}.is_leap(); }
int daysInMonth(long long y, int m)
{
  using namespace std::chrono;
  return int(unsigned(year_month_day_last{year{int(y)}, month_day_last{month{unsigned(m)}}}.day()));
}
template <typename D> std::string formatRfc3339(const std::chrono::time_point<std::chrono::system_clock, D>& t, int fractionDigits = 0)
{
  using namespace std::chrono;
  if (fractionDigits < 0 || fractionDigits > 7)
    throw InvalidArgument("fraction digits");
  CivilPoint p = time_point_cast<CivilTicks>(t);
  int y = int(year_month_day{floor<days>(p)}.year());
  if (y < 0 || y > 9999)
    throw OutOfRange("year");
  auto s = floor<seconds>(p);
  std::string out = std::format("{:%Y-%m-%dT%H:%M:%S}", s);
  if (fractionDigits > 0)
  {
    long long ticks = (p - s).count();
    for (int i = fractionDigits; i < 7; ++i)
      ticks /= 10;
    out += "." + std::format("{:0{}}", ticks, fractionDigits);
  }
  return out + "Z";
}
template <typename D> std::string formatHttpDate(const std::chrono::time_point<std::chrono::system_clock, D>& t)
{
  using namespace std::chrono;
  CivilPoint p = time_point_cast<CivilTicks>(t);
  int y = int(year_month_day{floor<days>(p)}.year());
  if (y < 0 || y > 9999)
    throw OutOfRange("year");
  return std::format("{:%a, %d %b %Y %H:%M:%S GMT}", floor<seconds>(p));
}
// tm1: time against std::chrono, the parts ch1 left out: Ratio arithmetic and comparisons
// (reduced, large terms), durationCast between unsigned, narrow and floating reps and periods that
// do not divide, floor/ceil/round of floating durations, abs, ++/--, zero/min/max, Weeks/Months/
// Years, HoursMinutesSeconds (hh_mm_ss) of every sign and precision, and the calendar (toCivilTime,
// fromCivilTime, daysFromCivil, daysInMonth, isLeapYear, formatRfc3339, formatHttpDate) against
// C++20's year_month_day, hh_mm_ss and std::format on edge instants.
template <typename D> void count(const char* label, const D& d) { printf("%s: %lld\n", label, (long long)d.count()); }
template <typename D> void countu(const char* label, const D& d) { printf("%s: %llu\n", label, (unsigned long long)d.count()); }
template <typename D> void countf(const char* label, const D& d) { printf("%s: %.17g\n", label, (double)d.count()); }
template <typename R> void ratio(const char* label) { printf("%s: %lld/%lld\n", label, (long long)R::num, (long long)R::den); }
template <typename D> void hms(const char* label, D d)
{
  HoursMinutesSeconds<D> x(d);
  printf("%s: neg %d h %lld m %lld s %lld sub %.17g back %.17g\n", label, (int)x.is_negative(), (long long)x.hours().count(), (long long)x.minutes().count(),
         (long long)x.seconds().count(), (double)x.subseconds().count(), (double)x.to_duration().count());
}
using Tp = TimePoint<SystemClock, Duration<long long, Ratio<1, 10000000>>>;
Tp at(long long ticks) { return Tp(Duration<long long, Ratio<1, 10000000>>(ticks)); }
void civil(long long ticks)
{
  CivilTime c = toCivilTime(at(ticks));
  printf("%lld: %d-%d-%d %d:%d:%d.%09d wd %d", ticks, c.year, c.month, c.day, c.hour, c.minute, c.second, c.nanosecond, c.weekday);
  auto back = fromCivilTime(c);
  printf(" back %s", !back.has_value() ? "empty" : (*back).time_since_epoch().count() == ticks ? "same" : "DIFFERENT");
  for (int digits : {0, 1, 3, 7})
  {
    try
    {
      printf(" [%s]", formatRfc3339(at(ticks), digits).c_str());
    }
    catch (const OutOfRange&)
    {
      printf(" [out of range]");
    }
  }
  try
  {
    printf(" [%s]\n", formatHttpDate(at(ticks)).c_str());
  }
  catch (const OutOfRange&)
  {
    printf(" [out of range]\n");
  }
}
void fromFields(int y, int mo, int d, int h, int mi, int s, int ns)
{
  CivilTime c;
  c.year = y, c.month = mo, c.day = d, c.hour = h, c.minute = mi, c.second = s, c.nanosecond = ns;
  auto t = fromCivilTime(c);
  if (!t.has_value())
    printf("from %d-%d-%d %d:%d:%d.%d: empty\n", y, mo, d, h, mi, s, ns);
  else
  {
    CivilTime r = toCivilTime(*t);
    printf("from %d-%d-%d %d:%d:%d.%d: %lld -> %d-%d-%d %d:%d:%d.%d wd %d\n", y, mo, d, h, mi, s, ns, (long long)(*t).time_since_epoch().count(), r.year,
           r.month, r.day, r.hour, r.minute, r.second, r.nanosecond, r.weekday);
  }
}

int main()
{
  // Ratio: reduced, signs, arithmetic and comparisons, terms near 10^12 and 10^18
  ratio<RatioAdd<Ratio<1, 2>, Ratio<1, 3>>>("1/2 + 1/3");
  ratio<RatioSubtract<Ratio<1, 2>, Ratio<1, 3>>>("1/2 - 1/3");
  ratio<RatioSubtract<Ratio<1, 3>, Ratio<1, 2>>>("1/3 - 1/2");
  ratio<RatioMultiply<Ratio<2, 3>, Ratio<9, 4>>>("2/3 * 9/4");
  ratio<RatioDivide<Ratio<2, 3>, Ratio<-4, 9>>>("2/3 / -4/9");
  ratio<RatioAdd<Ratio<1, 6>, Ratio<1, 10>>>("1/6 + 1/10");
  ratio<RatioAdd<Ratio<0, 5>, Ratio<0, 7>>>("0/5 + 0/7");
  ratio<Ratio<0, -3>>("0/-3");
  ratio<Ratio<-6, -4>>("-6/-4");
  ratio<RatioMultiply<Ratio<1000000000, 7>, Ratio<7, 1000000000>>>("1e9/7 * 7/1e9");
  ratio<RatioDivide<Ratio<1, 1000000000>, Ratio<1, 1000000000>>>("nano / nano");
  printf("1/3 < 1/2: %d, 1/2 <= 1/2: %d, 2/3 > 3/5: %d, 2/4 == 1/2: %d\n", (int)ratioLess<Ratio<1, 3>, Ratio<1, 2>>,
         (int)ratioLessEqual<Ratio<1, 2>, Ratio<1, 2>>, (int)ratioGreater<Ratio<2, 3>, Ratio<3, 5>>, (int)ratioEqual<Ratio<2, 4>, Ratio<1, 2>>);
  printf("-1/2 < 1/3: %d, -1/2 < -1/3: %d\n", (int)ratioLess<Ratio<-1, 2>, Ratio<1, 3>>, (int)ratioLess<Ratio<-1, 2>, Ratio<-1, 3>>);
  // durationCast: periods that do not divide, signs, unsigned and narrow reps, floating reps
  for (long long v : {-10ll, -1ll, 0ll, 1ll, 2ll, 10ll, 1000000007ll})
  {
    printf("--- %lld\n", v);
    count("thirds to sevenths", durationCast<Duration<long long, Ratio<1, 7>>>(Duration<long long, Ratio<1, 3>>(v)));
    count("sevenths to thirds", durationCast<Duration<long long, Ratio<1, 3>>>(Duration<long long, Ratio<1, 7>>(v)));
    count("3/2 s to 5/3 s", durationCast<Duration<long long, Ratio<5, 3>>>(Duration<long long, Ratio<3, 2>>(v)));
    count("min to 7 s", durationCast<Duration<long long, Ratio<7>>>(Minutes(v)));
    countf("thirds as double s", durationCast<Duration<double>>(Duration<long long, Ratio<1, 3>>(v)));
    countf("ms as float s", durationCast<Duration<float>>(Milliseconds(v)));
    count("short ms from s", durationCast<Duration<short, Ratio<1, 1000>>>(Seconds(v)));
    count("int us from s", durationCast<Duration<int, Ratio<1, 1000000>>>(Seconds(v)));
  }
  countu("u64 us max to ms", durationCast<Duration<unsigned long long, Ratio<1, 1000>>>(Duration<unsigned long long, Ratio<1, 1000000>>(18446744073709551615ull)));
  countu("u64 us 2^63 to s", durationCast<Duration<unsigned long long>>(Duration<unsigned long long, Ratio<1, 1000000>>(9223372036854775808ull)));
  countu("u32 ms max to s", durationCast<Duration<unsigned>>(Duration<unsigned, Ratio<1, 1000>>(4294967295u)));
  countu("u64 thirds big to sevenths", durationCast<Duration<unsigned long long, Ratio<1, 7>>>(Duration<unsigned long long, Ratio<1, 3>>(2000000000000000000ull)));
  for (double v : {-2.5, -1.75, -0.5, -0.25, 0.25, 0.5, 1.5, 1.75, 2.5, 1e15 + 0.5, 0.1})
  {
    printf("--- %.17g s\n", v);
    count("cast s", durationCast<Seconds>(Duration<double>(v)));
    count("floor s", floor<Seconds>(Duration<double>(v)));
    count("ceil s", ceil<Seconds>(Duration<double>(v)));
    count("round s", round<Seconds>(Duration<double>(v)));
    count("round ms", round<Milliseconds>(Duration<double>(v)));
    countf("float ms", Duration<float, Ratio<1, 1000>>(Duration<float>(float(v))));
    countf("float ms to float min", durationCast<Duration<float, Ratio<60>>>(Duration<float, Ratio<1, 1000>>(float(v * 1000))));
    countf("floor float s", floor<Duration<double>>(Duration<double, Ratio<1, 1000>>(v * 1000)));
    countf("abs", abs(Duration<double>(v)));
  }
  // abs, ++/--, compound arithmetic, zero/min/max, double factors
  count("abs -5 s", abs(Seconds(-5)));
  count("abs 5 s", abs(Seconds(5)));
  Seconds s(5);
  Seconds old = s++;
  count("s++ old", old);
  count("s++ new", s);
  old = s--;
  count("s-- old", old);
  count("--s", --s);
  count("++s", ++s);
  s += Seconds(10);
  count("+= 10 s", s);
  s -= Seconds(3);
  count("-= 3 s", s);
  s *= 3;
  count("*= 3", s);
  s /= 4;
  count("/= 4", s);
  Milliseconds ms(1500);
  ms += Seconds(2);
  count("1500 ms += 2 s", ms);
  count("zero", Seconds::zero());
  count("ms max", Milliseconds::max());
  count("ms min", Milliseconds::min());
  countf("double max", Duration<double>::max());
  countf("3 s * 1.5", Seconds(3) * 1.5);
  countf("1.5 * 3 s", 1.5 * Seconds(3));
  countf("3 s / 2.0", Seconds(3) / 2.0);
  printf("1.0 s / 250 ms (double): %.17g\n", (double)(Duration<double>(1.0) / Duration<double, Ratio<1, 1000>>(250)));
  printf("7 s / 2000 ms: %lld\n", (long long)(Seconds(7) / Milliseconds(2000)));
  count("7 s % 2000 ms", Seconds(7) % Milliseconds(2000));
  count("-7 s % 2000 ms", Seconds(-7) % Milliseconds(2000));
  count("1/2 s + 1/3 s", Duration<int, Ratio<1, 2>>(1) + Duration<int, Ratio<1, 3>>(1));
  printf("1/2 s == 3/6 s: %d, 1/2 s < 1/3 s * 2: %d\n", (int)(Duration<int, Ratio<1, 2>>(1) == Duration<int, Ratio<1, 6>>(3)),
         (int)(Duration<int, Ratio<1, 2>>(1) < Duration<int, Ratio<1, 3>>(2)));
  printf("treat as floating: int %d double %d float %d\n", (int)treatAsFloatingPoint<int>, (int)treatAsFloatingPoint<double>, (int)treatAsFloatingPoint<float>);
  // the calendar's lengths
  count("1 week in days", durationCast<Days>(Weeks(1)));
  count("1 year in s", durationCast<Seconds>(Years(1)));
  count("1 month in s", durationCast<Seconds>(Months(1)));
  count("12 months == 1 year", Months(12) - durationCast<Months>(Years(1)));
  count("1 year in days (truncated)", durationCast<Days>(Years(1)));
  count("1 year in weeks", durationCast<Weeks>(Years(1)));
  count("1 year in h", durationCast<Hours>(Years(1)));
  count("400 years in days", durationCast<Days>(Years(400)));
  // HoursMinutesSeconds
  for (long long v : {0ll, 1ll, 59999ll, 3600000ll, 3661001ll, 86399999ll, 360000000ll, -1ll, -3661001ll, -86400000ll})
  {
    char label[64];
    snprintf(label, 64, "hms %lld ms", v);
    hms(label, Milliseconds(v));
  }
  hms("hms 5400 s", Seconds(5400));
  hms("hms -5400 s", Seconds(-5400));
  hms("hms 90 min", Minutes(90));
  hms("hms -90 min", Minutes(-90));
  hms("hms 25 h", Hours(25));
  hms("hms 3723.75 s double", Duration<double>(3723.75));
  hms("hms -3723.75 s double", Duration<double>(-3723.75));
  hms("hms 1 ns", Nanoseconds(1));
  hms("hms 3723000000001 ns", Nanoseconds(3723000000001ll));
  hms("hms 7 thirds", Duration<long long, Ratio<1, 3>>(7));
  hms("hms 100 h us", Microseconds(360000000001ll));
  // the calendar on edge instants (100 ns ticks since 1970)
  long long day = 864000000000ll;
  for (long long t : {0ll, 1ll, -1ll, -10000000ll, -10000001ll, day - 1, -day, -day - 1, 951782400ll * 10000000, 951868800ll * 10000000 - 1, 4107542399ll * 10000000,
                      253402300799ll * 10000000 + 9999999, 253402300800ll * 10000000, -62135596800ll * 10000000, -62167219200ll * 10000000,
                      -62167219200ll * 10000000 - 1, 1234567890123456789ll, -1234567890123456789ll, 9223372036854775807ll})
    civil(t);
  // fromCivilTime: fields out of range, leap days, the years it accepts
  fromFields(2024, 2, 29, 0, 0, 0, 0);
  fromFields(2023, 2, 29, 0, 0, 0, 0);
  fromFields(1900, 2, 29, 0, 0, 0, 0);
  fromFields(2000, 2, 29, 23, 59, 59, 999999999);
  fromFields(2026, 4, 31, 0, 0, 0, 0);
  fromFields(2026, 0, 1, 0, 0, 0, 0);
  fromFields(2026, 13, 1, 0, 0, 0, 0);
  fromFields(2026, 1, 0, 0, 0, 0, 0);
  fromFields(2026, 1, 1, 24, 0, 0, 0);
  fromFields(2026, 1, 1, 0, 60, 0, 0);
  fromFields(2026, 1, 1, 0, 0, 60, 0);
  fromFields(2026, 1, 1, 0, 0, 0, 1000000000);
  fromFields(2026, 1, 1, 0, 0, 0, -1);
  fromFields(2026, 1, 1, -1, 0, 0, 0);
  fromFields(2026, 1, 1, 0, 0, 0, 199);
  fromFields(0, 1, 1, 0, 0, 0, 0);
  fromFields(-1, 12, 31, 23, 59, 59, 0);
  fromFields(-4, 2, 29, 0, 0, 0, 0);
  fromFields(29000, 12, 31, 23, 59, 59, 999999900);
  fromFields(29001, 1, 1, 0, 0, 0, 0);
  fromFields(-29000, 1, 1, 0, 0, 0, 0);
  fromFields(-29001, 12, 31, 0, 0, 0, 0);
  fromFields(-28000, 6, 15, 12, 0, 0, 0);
  fromFields(-27258, 1, 1, 0, 0, 0, 0);
  fromFields(-27257, 1, 1, 0, 0, 0, 0);
  fromFields(-27000, 1, 1, 0, 0, 0, 0);
  fromFields(27000, 1, 1, 0, 0, 0, 0);
  // daysFromCivil, daysInMonth, isLeapYear
  for (int y : {-401, -400, -100, -4, -1, 0, 1, 4, 100, 1600, 1900, 1969, 1970, 2000, 2024, 2100, 9999, 10000})
  {
    printf("year %d: leap %d, feb %d, days to jan 1 %lld, to dec 31 %lld\n", y, (int)isLeapYear(y), daysInMonth(y, 2), (long long)daysFromCivil(y, 1, 1),
           (long long)daysFromCivil(y, 12, 31));
  }
  for (int m = 1; m <= 12; ++m)
    printf("%d ", daysInMonth(2023, m));
  printf("\n");
  try
  {
    formatRfc3339(at(0), 8);
    printf("8 digits: no throw\n");
  }
  catch (const InvalidArgument&)
  {
    printf("8 digits: invalid argument\n");
  }
  try
  {
    formatRfc3339(at(0), -1);
    printf("-1 digits: no throw\n");
  }
  catch (const InvalidArgument&)
  {
    printf("-1 digits: invalid argument\n");
  }
  return 0;
}
