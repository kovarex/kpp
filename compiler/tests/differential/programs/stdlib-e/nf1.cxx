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
#include <cstring>
#include <cmath>
// kpp's rules in C: the shortest %.Ng that strtod (strtof) reads back; inf, -inf and nan as kpp writes them.
// The shortest digits that read back (p of them, decimal exponent x), written positionally for
// -4 <= x < 16 and as %e otherwise (text.md: %g's form, but positional up to 1e16).
std::string laidOut(double d, int p)
{
  char buf[512];
  snprintf(buf, 512, "%.*e", p - 1, d);
  int x = atoi(strchr(buf, 'e') + 1);
  if (x >= -4 && (x < 16 || x < p))
    snprintf(buf, 512, "%.*f", p - 1 - x > 0 ? p - 1 - x : 0, d);
  return buf;
}
std::string shortestDouble(double d)
{
  if (std::isinf(d))
    return d < 0 ? "-inf" : "inf";
  if (d != d)
    return "nan";
  if (d == 0)
    return std::signbit(d) ? "-0" : "0";
  char buf[64];
  int p = 1;
  for (; p < 17; ++p)
  {
    snprintf(buf, 64, "%.*e", p - 1, d);
    if (strtod(buf, nullptr) == d)
      break;
  }
  return laidOut(d, p);
}
std::string shortestFloat(float x)
{
  if (std::isinf(x))
    return x < 0 ? "-inf" : "inf";
  if (x == 0)
    return std::signbit(x) ? "-0" : "0";
  char buf[64];
  int p = 1;
  for (; p < 9; ++p)
  {
    snprintf(buf, 64, "%.*e", p - 1, (double)x);
    if (strtof(buf, nullptr) == x)
      break;
  }
  // A float's text in positional form is its exact digits (`15000000512`): laid out from the
  // double's value at p digits is the same text for the exponent and the scientific form.
  return laidOut((double)x, p);
}
std::string& operator<<(std::string& s, double d) { return s += shortestDouble(d); }
std::string& operator<<(std::string& s, float x) { return s += shortestFloat(x); }
std::string& operator<<(std::string& s, const std::string& t) { return s += t; }
std::string formatted(const char* spec, int n, double d)
{
  if (std::isinf(d))
    return d < 0 ? "-inf" : "inf";
  char buf[512];
  snprintf(buf, 512, spec, n, d);
  return buf;
}
std::string fixed(double d, int n) { return formatted("%.*f", n, d); }
std::string significant(double d, int n) { return formatted("%.*g", n, d); }
std::string scientific(double d, int n) { return formatted("%.*e", n, d); }
// Numbers as text against C: a double or float appended to a String is the shortest text that reads
// back to the same value, in %g's form at that many digits (text.md), checked against the smallest
// %.Ng that strtod/strtof read back, and that it does read back; fixed, significant and scientific
// against %.Nf, %.Ng and %.Ne, on chosen values (ties, subnormals, the extremes, powers of ten) and
// on random bit patterns.
unsigned long long state = 88172645463325252ull;
unsigned long long nextBits()
{
  state ^= state << 13;
  state ^= state >> 7;
  state ^= state << 17;
  return state;
}
unsigned long long hash = 1469598103934665603ull;
void mixText(const String& s)
{
  for (size_t i = 0; i < s.size(); ++i)
    hash = (hash ^ (unsigned char)s[i]) * 1099511628211ull;
  hash = (hash ^ 0xff) * 1099511628211ull;
}
int roundTripFailures = 0;
void checkDouble(double d, bool show)
{
  String text;
  text << d;
  if (d == d && strtod(text.c_str(), nullptr) != d)
    ++roundTripFailures;
  String f2, f17, g6, g17, e3, e0;
  f2 << fixed(d, 2);
  f17 << fixed(d, 17);
  g6 << significant(d, 6);
  g17 << significant(d, 17);
  e3 << scientific(d, 3);
  e0 << scientific(d, 0);
  mixText(text);
  mixText(f2);
  mixText(g6);
  mixText(g17);
  mixText(e3);
  mixText(e0);
  if (d > -1e30 && d < 1e30)
    mixText(f17);
  if (show)
    printf("%-26s %-12s %-12s %-24s %-12s\n", text.c_str(), f2.c_str(), g6.c_str(), g17.c_str(), e3.c_str());
}
void checkFloat(float x, bool show)
{
  String text;
  text << x;
  if (x == x && strtof(text.c_str(), nullptr) != x)
    ++roundTripFailures;
  mixText(text);
  if (show)
    printf("float %s\n", text.c_str());
}

int main()
{
  double chosen[] = {0.0, -0.0, 0.1, 0.1 + 0.2, 1.0 / 3, 2.5, 3.5, -2.5, 0.125, 0.375, 1e-5, 1e-4, 9.9999e-5, 123456.0, 1e15, 1e16, 9007199254740993.0,
                     1.2345678901234568e17, 5e-324, 2.2250738585072014e-308, 1.7976931348623157e308, 0.005, 0.015, 0.025, 1.005, 2.675, 1e21, 1e22, 1e23,
                     123.456, 100.0, 4.35, 0.3, 9.5, 99.995, 1.0 / 0.0 * 1.0, -1.0 / 0.0 * 1.0};
  for (double d : chosen)
    checkDouble(d, true);
  float floats[] = {0.1f, 1.0f / 3, 16777216.0f, 16777217.0f, 1.5e10f, 3.4028235e38f, 1e-45f, 1.17549435e-38f, 0.3f, 2.5f, 100.0f, 1e-5f};
  for (float x : floats)
    checkFloat(x, true);
  for (int i = 0; i < 200000; ++i)
  {
    unsigned long long bits = nextBits();
    double d;
    memcpy(&d, &bits, 8);
    if (d != d)
      continue;
    checkDouble(d, false);
    unsigned b32 = (unsigned)bits;
    float x;
    memcpy(&x, &b32, 4);
    if (x == x)
      checkFloat(x, false);
  }
  for (int e = -320; e <= 308; ++e)
  {
    char text[32];
    snprintf(text, 32, "1e%d", e);
    checkDouble(strtod(text, nullptr), false);
    snprintf(text, 32, "5e%d", e);
    checkDouble(strtod(text, nullptr), false);
  }
  for (int i = 0; i < 100000; ++i)
  {
    // Decimal-looking values: k / 1000 and k / 8, where ties are exact.
    checkDouble(i / 1000.0, false);
    checkDouble(i / 8.0 + 0.0625, false);
  }
  printf("hash %016llx, round-trip failures %d\n", hash, roundTripFailures);
  return 0;
}
