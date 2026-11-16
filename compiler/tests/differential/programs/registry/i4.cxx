// battery i4 (C++ side): the texts docs/stdlib/text.md specifies, computed independently: shortest
// round-trip digits from std::to_chars, positional between 1e-4 and 1e16, the pieces through printf.
#include <stdio.h>
#include <charconv>
#include <string>
#include <string.h>
#include <stdint.h>
#include <math.h>
template <typename F> std::string shortest(F v)
{
  if (v != v) return "nan";
  if (isinf(v)) return v > 0 ? "inf" : "-inf";
  if (v == 0) return signbit(v) ? "-0" : "0";
  char sci[64];
  auto r = std::to_chars(sci, sci + 64, v, std::chars_format::scientific);
  *r.ptr = 0;
  int exponent = atoi(strchr(sci, 'e') + 1);
  if (exponent < -4 || exponent >= 16) return sci;
  char fix[400];
  r = std::to_chars(fix, fix + 400, v, std::chars_format::fixed);
  *r.ptr = 0;
  return fix;
}
int main()
{
  volatile double zero = 0;
  double ds[] = {0.1, 0.1 + 0.2, 1.0 / 3, 100, 1e15, 9999999999999998.0, 1e16, 1.5e16, 123456789012345680.0, 1e22,
    1e-4, 1.5e-5, 0.00012345, 5e-324, 1.7976931348623157e308, -0.0, 2.5, -1234.5678, 9007199254740993.0, 1e21,
    zero / zero, 1 / zero, -1 / zero, 4.35, 0.3, 1e-7, 123456.0, 1e100};
  for (double d : ds) printf("d %s\n", shortest(d).c_str());
  float fs[] = {0.1f, 1.0f / 3, 16777216.0f, 3.4028235e38f, 1e-45f, 123456.79f, 1e9f, 1e10f, 1.5e10f, 1e15f, 1e16f, 1e17f,
    1.23456789e12f, 0.0001f, 1e-5f, -0.0f, 7.0f, 2.5e-3f};
  for (float f : fs) printf("f %s\n", shortest(f).c_str());
  printf("i %lld %llu %d %d %d %d\n", (long long)INT64_MIN, (unsigned long long)UINT64_MAX, (int)(int8_t)-5, (int)(uint8_t)200, (int)(int16_t)-300, (int)(uint16_t)65000);
  printf("hex %x %x %x %llx %X %04X %x %o %08o\n", -1, (unsigned)(uint16_t)(int16_t)-1, (unsigned)(uint8_t)(int8_t)-1, (unsigned long long)-1LL, 0xABCu, 0xAu, 0u, 0644u, 8u);
  printf("fixed %.0f %.0f %.2f %.2f %.20f %.3f %.0f %.1f\n", 2.5, 3.5, 0.125, 0.375, 0.1, -0.0005, 1e20, 1e-300);
  printf("sig %g %.6g %.4g %g %g %.1g %g %.3g\n", 1234567.0, 100.0, 0.0001234567, 1e-5, 123456.0, 0.05, 0.0, 999.9);
  printf("sci %.2e %e %.0e %.3e\n", 1234.5, 0.0, 5.5, -1e-300);
  printf("sign %+.1f %+d %+d %+g %+.2f\n", 1.25, 3, -3, 0.0, -0.001);
  printf("align [%5d] [%-5d] [%5s] [%-6s] [%2d] [%*d] [%*d]\n", 42, 42, "ab", "ab", 12345, 6, 7, -4, 7);
  printf("zero %04d %+04d %08.2f %05x %03d %06.1f\n", -5, 3, -1.5, 0xABu, 12345, 2.25);
  return 0;
}
