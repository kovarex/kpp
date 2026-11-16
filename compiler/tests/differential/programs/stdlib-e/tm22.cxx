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
template <typename R> void ratio(const char* label) { printf("%s: %lld/%lld\n", label, (long long)R::num, (long long)R::den); }
int main()
{
  ratio<RatioMultiply<Ratio<1000000000000, 1>, Ratio<1, 1000000000000>>>("tera * pico");
  return 0;
}
