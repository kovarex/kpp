#include <stdio.h>
#include <cmath>
using namespace std;
int main()
{
  volatile double zero = 0;
  double sub = 4.9e-324 * (1 + zero), small = 2.2250738585072014e-308 * (1 + zero), nan = zero / zero;
  float subf = 1e-45f * (float)(1 + zero);
  printf("isnormal %d %d %d %d %d %d\n", (int)isnormal(sub), (int)isnormal(small), (int)isnormal(1.0), (int)isnormal(0.0), (int)isnormal(subf), (int)isnormal(nan));
  printf("signbit %d %d %d %d\n", (int)signbit(-nan), (int)signbit(nan), (int)signbit(-0.0), (int)signbit(-1e-300));
  printf("round %g %g %g %g %g %g\n", round(-2.5), round(2.5), round(-0.4), floor(-0.5), ceil(-0.5), trunc(-1.7));
  printf("fmod %g %g %g %g\n", fmod(-7.5, 2.0), fmod(7.5, -2.0), remainder(7.5, 2.0), remainder(-7.5, 2.0));
  printf("float %.9g %.9g %.9g %.9g\n", (double)sqrt(2.0f), (double)floor(-1.5f), (double)round(-1.5f), (double)ceil(1.0000001f));
  printf("abs %d %lld %g %g\n", abs(-5), (long long)abs(-5000000000LL), abs(-0.5), (double)abs(-2.5f));
  printf("misc %g %g %d %g\n", hypot(3.0, 4.0), cbrt(-27.0), (int)lround(-2.5), copysign(3.0, -0.0));
  return 0;
}
