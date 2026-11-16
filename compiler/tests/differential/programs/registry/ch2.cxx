#include <stdio.h>
#include <chrono>
#include <ratio>
using namespace std::chrono;
template <long long N, long long D = 1> using Ratio = std::ratio<N, D>;
template <class A, class B> using RatioAdd = std::ratio_add<A, B>; template <class A, class B> using RatioSubtract = std::ratio_subtract<A, B>; template <class A, class B> using RatioMultiply = std::ratio_multiply<A, B>; template <class A, class B> using RatioDivide = std::ratio_divide<A, B>;
template <class A, class B> constexpr bool ratioLess = std::ratio_less_v<A, B>; template <class A, class B> constexpr bool ratioEqual = std::ratio_equal_v<A, B>; template <class A, class B> constexpr bool ratioGreaterEqual = std::ratio_greater_equal_v<A, B>; template <class A, class B> constexpr bool ratioNotEqual = std::ratio_not_equal_v<A, B>;
using Seconds = seconds; using Milliseconds = milliseconds; using Days = days; using Weeks = weeks; using Years = years; using Months = months; using DoubleSeconds = duration<double>;
template <class R> constexpr bool treatAsFloatingPoint = treat_as_floating_point_v<R>;
template <class D> D absD(D d) { return std::chrono::abs(d); }
template <class D> using Hms = hh_mm_ss<D>;
template <class To, class D> To durationCast(D d) { return duration_cast<To>(d); }
int main()
{
  using R1 = Ratio<1, 6>;
  using R2 = Ratio<3, 4>;
  printf("ratio %lld/%lld %lld/%lld %lld/%lld %lld/%lld\n", (long long)RatioAdd<R1, R2>::num, (long long)RatioAdd<R1, R2>::den, (long long)RatioSubtract<R1, R2>::num,
         (long long)RatioSubtract<R1, R2>::den, (long long)RatioMultiply<R1, R2>::num, (long long)RatioMultiply<R1, R2>::den, (long long)RatioDivide<R1, R2>::num,
         (long long)RatioDivide<R1, R2>::den);
  printf("compare %d %d %d %d\n", (int)ratioLess<R1, R2>, (int)ratioEqual<Ratio<2, 4>, Ratio<1, 2>>, (int)ratioGreaterEqual<R1, R1>, (int)ratioNotEqual<R1, R2>);
  printf("abs %lld %lld %g\n", (long long)absD(Seconds(-5)).count(), (long long)absD(Milliseconds(7)).count(), absD(DoubleSeconds(-2.5)).count());
  printf("float %d %d\n", (int)treatAsFloatingPoint<double>, (int)treatAsFloatingPoint<long long>);
  printf("calendar %lld %lld %lld %lld\n", (long long)durationCast<Seconds>(Weeks(1)).count(), (long long)durationCast<Seconds>(Years(1)).count(),
         (long long)durationCast<Seconds>(Months(1)).count(), (long long)durationCast<Days>(Years(4)).count());
  long long values[] = {-1, 0, 3661005, -90061001, 86399999};
  for (long long v : values)
  {
    Hms<Milliseconds> t{Milliseconds(v)};
    printf("hms %d %lld %lld %lld %lld %lld\n", (int)t.is_negative(), (long long)t.hours().count(), (long long)t.minutes().count(), (long long)t.seconds().count(),
           (long long)t.subseconds().count(), (long long)t.to_duration().count());
  }
  Hms<Seconds> whole{Seconds(-7322)};
  printf("hms s %lld %lld %lld %lld\n", (long long)whole.hours().count(), (long long)whole.minutes().count(), (long long)whole.seconds().count(), (long long)whole.to_duration().count());
  return 0;
}
