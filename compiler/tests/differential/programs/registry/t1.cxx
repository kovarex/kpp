#include <stdio.h>
#include <chrono>
using Nanoseconds = std::chrono::nanoseconds; using Microseconds = std::chrono::microseconds; using Milliseconds = std::chrono::milliseconds; using Seconds = std::chrono::seconds;
template <class To, class D> To floorD(D d) { return std::chrono::floor<To>(d); }
template <class To, class D> To ceilD(D d) { return std::chrono::ceil<To>(d); }
template <class To, class D> To roundD(D d) { return std::chrono::round<To>(d); }
template <class To, class D> To durationCast(D d) { return std::chrono::duration_cast<To>(d); }
int main()
{
  long long ns[] = {-1, 1, -1500000000LL, 1500000000LL, -2500000000LL, 2500000000LL, -100000000000000001LL, 100000000000000001LL, -9007199254740993LL, 4611686018427387905LL, 499999999LL, -500000000LL, 500000000LL};
  for (long long v : ns)
  {
    Nanoseconds d(v);
    printf("%lld: floor us %lld s %lld | ceil us %lld s %lld | round s %lld ms %lld | cast us %lld\n", v, (long long)floorD<Microseconds>(d).count(), (long long)floorD<Seconds>(d).count(),
           (long long)ceilD<Microseconds>(d).count(), (long long)ceilD<Seconds>(d).count(), (long long)roundD<Seconds>(d).count(), (long long)roundD<Milliseconds>(d).count(),
           (long long)durationCast<Microseconds>(d).count());
  }
  Milliseconds m(-2500);
  printf("ms %lld %lld %lld\n", (long long)roundD<Seconds>(m).count(), (long long)floorD<Seconds>(m).count(), (long long)ceilD<Seconds>(m).count());
  return 0;
}
