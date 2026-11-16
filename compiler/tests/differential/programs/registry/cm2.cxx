#include <stdio.h>
#include <cmath>
using namespace std;
int main()
{
  volatile double zero = 0;
  double ds[] = {-2.5, -0.5, 0.0, 0.5, 1.5, 2.5, 3.7, 1e-310, 100.0, -1e300};
  for (double d : ds)
  {
    int q = 0;
    double r = remquo(d, 0.75, &q);
    printf("%a: %a %a %ld %lld %lld %a %d %d %a %a %a %a %a %a %a %a %a %a %a\n", d, nearbyint(d), rint(d), lrint(d / 1e290 > 1 ? 0.0 : d), llrint(d), llround(d), r, q, ilogb(d == 0 ? 1.0 : d), logb(d == 0 ? 1.0 : d),
           scalbn(d, 3), fma(d, 1.5, 0.25), fmax(d, 0.5), fmin(d, 0.5), fdim(d, 0.5), exp2(d / 100), expm1(d / 100), log1p(fabs(d) / 100), erf(d), erfc(d));
    printf("  %a %a %a\n", tgamma(fabs(d) + 0.25), lgamma(fabs(d) + 0.25), nexttoward(d, 1.0));
  }
  float fs[] = {-2.5f, 0.5f, 1.5f, 3.7f, 1e-40f};
  for (float f : fs)
  {
    int q = 0;
    printf("%a: %a %a %ld %lld %a %d %a %a %a %a %a %a %a %a %a %a %a\n", (double)f, (double)nearbyint(f), (double)rint(f), lrint(f), llround(f), (double)remquo(f, 0.75f, &q), q,
           (double)scalbn(f, 3), (double)fma(f, 1.5f, 0.25f), (double)fmax(f, 0.5f), (double)fdim(f, 0.5f), (double)exp2(f), (double)expm1(f), (double)log1p(fabs(f)), (double)erf(f),
           (double)tgamma(fabs(f) + 0.25f), (double)lgamma(fabs(f) + 0.25f), (double)nexttoward(f, 1.0));
    printf("  fabs %d %a\n", (int)sizeof(fabs(f)), (double)fabs(f));
  }
  printf("ints %a %a %ld %d %a\n", tgamma(5), erf(1), lrint(7), ilogb(1024), fmax(3, 2.5));
  return 0;
}
