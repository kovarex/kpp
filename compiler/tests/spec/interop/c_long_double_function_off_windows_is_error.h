/* For interop/c_long_double_function_off_windows_is_error.kpp: C functions of long double, as math.h's floorl
   and modfl, one through a typedef. */
typedef long double spec_real;
long double specHalfOf(long double x);
double specWholePart(double x, long double* whole);
spec_real specThirdOf(spec_real x);
