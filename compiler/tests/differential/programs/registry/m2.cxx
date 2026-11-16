#include <stdio.h>
#include <cmath>
using namespace std;
int main()
{
  volatile double zero = 0;
  double nz = -zero;
  float nzf = (float)-zero;
  double negNan = -(zero / zero);
  printf("abs %g %g %g %d\n", abs(nz), (double)abs(nzf), 1 / abs(nz), (int)signbit(abs(negNan)));
  printf("isnormal %d %d %d\n", (int)isnormal(1.17549435e-38f * (float)(1 + zero)), (int)isnormal(1.1754942e-38f * (float)(1 + zero)), (int)isnormal(-4.9e-324 * (1 + zero)));
  return 0;
}
