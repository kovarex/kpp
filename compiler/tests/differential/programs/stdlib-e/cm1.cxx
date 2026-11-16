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
#include <cmath>
#include <cstring>
#include <limits>
using namespace std;
template <typename T> using Limits = std::numeric_limits<T>;
// <cmath>: each function kpp has, on edge values (±0, ±1, halves, subnormals, the largest, ±inf, NaN),
// its result as bits (the sign of 0 and NaN's sign kept); the float overloads' results as floats; an
// integer argument's (std: double).
unsigned long long bitsOf(double v)
{
  unsigned long long out = 0;
  memcpy(&out, &v, 8);
  return out;
}
unsigned bitsOfFloat(float v)
{
  unsigned out = 0;
  memcpy(&out, &v, 4);
  return out;
}
void show1(const char* name, double x, double r) { printf("%-9s %016llx -> %016llx\n", name, bitsOf(x), bitsOf(r)); }
void show2(const char* name, double x, double y, double r) { printf("%-9s %016llx %016llx -> %016llx\n", name, bitsOf(x), bitsOf(y), bitsOf(r)); }

int main()
{
  double inf = 1.0 / 0.0 * 1.0;
  double values[] = {0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 1.5, 2.5, -2.5, 3.5, 0.49999999999999994, 4503599627370495.5, 1e-310, -1e-310, 1.7976931348623157e308,
                     2.2250738585072014e-308, inf, -inf, Limits<double>::quiet_NaN(), 7.0, -7.0, 0.1, 100.0, 1e16, 6.283185307179586};
  for (double x : values)
  {
    printf("--- %016llx: nan %d inf %d finite %d normal %d signbit %d\n", bitsOf(x), (int)isnan(x), (int)isinf(x), (int)isfinite(x), (int)isnormal(x),
           (int)signbit(x));
    show1("floor", x, floor(x));
    show1("ceil", x, ceil(x));
    show1("trunc", x, trunc(x));
    show1("round", x, round(x));
    if (isfinite(x) && fabs(x) < 1e18)
      printf("lround    -> %ld\n", (long)lround(x));
    show1("fabs", x, fabs(x));
    show1("sqrt", x, sqrt(x));
    show1("cbrt", x, cbrt(x));
    show1("log2", x, log2(x));
    show1("exp", x, exp(x));
    show1("log", x, log(x));
    show1("sin", x, sin(x));
    show1("cos", x, cos(x));
    show1("atan", x, atan(x));
    int e = 0;
    double m = frexp(x, &e);
    printf("frexp     -> %016llx exp %d\n", bitsOf(m), isfinite(x) ? e : 0);
    double whole = 0;
    double part = modf(x, &whole);
    printf("modf      -> %016llx %016llx\n", bitsOf(part), bitsOf(whole));
    show1("ldexp 3", x, ldexp(x, 3));
    show1("ldexp-1100", x, ldexp(x, -1100));
    show2("fmod", x, 0.75, fmod(x, 0.75));
    show2("remainder", x, 0.75, remainder(x, 0.75));
    show2("copysign", 2.0, x, copysign(2.0, x));
    show2("nextafter", x, inf, nextafter(x, inf));
    show2("nextafter", x, -inf, nextafter(x, -inf));
    show2("hypot", x, 3.0, hypot(x, 3.0));
    show2("pow", x, 0.5, pow(x, 0.5));
    show2("pow", 2.0, x, pow(2.0, x));
    show2("atan2", x, -1.0, atan2(x, -1.0));
  }
  float f = 2.0f;
  printf("sqrt(float) size %d bits %08x\n", (int)sizeof(sqrt(f)), bitsOfFloat(sqrt(f)));
  printf("floor(float) size %d, round(float) size %d, fabs(float) size %d\n", (int)sizeof(floor(f)), (int)sizeof(round(f)), (int)sizeof(fabs(f)));
  printf("sqrt(int) size %d bits %016llx\n", (int)sizeof(sqrt(2)), bitsOf(sqrt(2)));
  printf("pow(int, int) size %d value %.17g\n", (int)sizeof(pow(2, 10)), (double)pow(2, 10));
  printf("abs(int) %d abs(long long) %lld abs(double) %.17g\n", abs(-5), (long long)abs(-5000000000LL), abs(-2.5));
  return 0;
}
