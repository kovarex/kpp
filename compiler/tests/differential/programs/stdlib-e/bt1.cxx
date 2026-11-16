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
#include <bit>
using std::rotr;
template <typename To, typename From> To bitCast(const From& f) { return std::bit_cast<To>(f); }
// <bit>: rotr against std::rotr for every count from -200 to 200 on edge values (u32 and u64), and
// bitCast against std::bit_cast between floats and integers (±0, subnormals, inf, NaN payloads),
// a struct and an integer, and a character array and an integer.
struct Halves
{
  unsigned short low;
  unsigned short high;
};
struct Chars
{
  char c[8];
};
int main()
{
  unsigned values32[] = {0u, 1u, 0x80000000u, 0x80000001u, 0x12345678u, 0xffffffffu, 0xfffffffeu};
  for (unsigned v : values32)
  {
    unsigned long long sum = 0;
    for (int c = -200; c <= 200; ++c)
      sum = sum * 1000003u + rotr(v, c);
    printf("rotr32 %08x: %016llx %08x %08x %08x %08x\n", v, sum, rotr(v, 1), rotr(v, -1), rotr(v, 33), rotr(v, -2147483647 - 1));
  }
  unsigned long long values64[] = {0ull, 1ull, 0x8000000000000000ull, 0x8000000000000001ull, 0x0123456789abcdefull, 0xffffffffffffffffull};
  for (unsigned long long v : values64)
  {
    unsigned long long sum = 0;
    for (int c = -200; c <= 200; ++c)
      sum = sum * 1000003u + rotr(v, c);
    printf("rotr64 %016llx: %016llx %016llx %016llx %016llx\n", v, sum, rotr(v, 1), rotr(v, -1), rotr(v, 65), rotr(v, 2147483647));
  }
  float floats[] = {0.0f, -0.0f, 1.0f, -1.5f, 1e-45f, 3.4028235e38f, 1.0f / 0.0f * 1.0f};
  for (float f : floats)
  {
    unsigned u = bitCast<unsigned>(f);
    printf("float %08x back %d\n", u, (int)(bitCast<unsigned>(bitCast<float>(u)) == u));
  }
  unsigned nans[] = {0x7fc00000u, 0xffc00001u, 0x7f800001u, 0x7fbfffffu};
  for (unsigned n : nans)
  {
    float f = bitCast<float>(n);
    printf("nan %08x isnan %d back %08x\n", n, (int)(f != f), bitCast<unsigned>(f));
  }
  double doubles[] = {0.0, -0.0, 1.0, 0.1, 5e-324, 1.7976931348623157e308};
  for (double d : doubles)
    printf("double %016llx\n", bitCast<unsigned long long>(d));
  unsigned long long dn = 0x7ff8000000000123ull;
  printf("double nan back %016llx\n", bitCast<unsigned long long>(bitCast<double>(dn)));
  Halves h = bitCast<Halves>(0x12345678u);
  printf("Halves %04x %04x back %08x\n", h.low, h.high, bitCast<unsigned>(h));
  Chars c = bitCast<Chars>(0x0102030405060708ull);
  printf("Chars %d %d %d back %016llx\n", c.c[0], c.c[7], (int)sizeof(Chars), bitCast<unsigned long long>(c));
  return 0;
}
