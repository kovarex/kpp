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
#include <limits>
#include <cstring>
template <typename T> using Limits = std::numeric_limits<T>;
// Limits<T> against std::numeric_limits<T>, every arithmetic type: the constants, and max, min,
// lowest (integers as numbers, floating values as their bits), epsilon, round_error, denorm_min.
template <typename T> unsigned long long bits(T v)
{
  unsigned long long out = 0;
  memcpy(&out, &v, sizeof v);
  return out;
}
template <typename T> void integer(const char* name)
{
  using L = Limits<T>;
  printf("%-18s size %d signed %d integer %d exact %d modulo %d bounded %d digits %d digits10 %d max_digits10 %d radix %d max %lld min %lld lowest %lld epsilon %lld\n", name,
         (int)sizeof(T), (int)L::is_signed, (int)L::is_integer, (int)L::is_exact, (int)L::is_modulo, (int)L::is_bounded, (int)L::digits, (int)L::digits10,
         (int)L::max_digits10, (int)L::radix, (long long)L::max(), (long long)L::min(), (long long)L::lowest(), (long long)L::epsilon());
}
template <typename T> void unsignedInteger(const char* name)
{
  using L = Limits<T>;
  printf("%-18s size %d signed %d modulo %d digits %d digits10 %d max %llu min %llu\n", name, (int)sizeof(T), (int)L::is_signed, (int)L::is_modulo,
         (int)L::digits, (int)L::digits10, (unsigned long long)L::max(), (unsigned long long)L::min());
}
template <typename T> void floating(const char* name)
{
  using L = Limits<T>;
  printf("%-8s signed %d integer %d exact %d inf %d qnan %d snan %d iec559 %d modulo %d digits %d digits10 %d max_digits10 %d exp %d..%d exp10 %d..%d\n", name,
         (int)L::is_signed, (int)L::is_integer, (int)L::is_exact, (int)L::has_infinity, (int)L::has_quiet_NaN, (int)L::has_signaling_NaN, (int)L::is_iec559,
         (int)L::is_modulo, (int)L::digits, (int)L::digits10, (int)L::max_digits10, (int)L::min_exponent, (int)L::max_exponent, (int)L::min_exponent10,
         (int)L::max_exponent10);
  printf("  max %016llx min %016llx lowest %016llx epsilon %016llx round_error %016llx denorm_min %016llx inf %016llx\n", bits(L::max()), bits(L::min()),
         bits(L::lowest()), bits(L::epsilon()), bits(L::round_error()), bits(L::denorm_min()), bits(L::infinity()));
  T q = L::quiet_NaN();
  printf("  quiet NaN is NaN %d\n", (int)(q != q));
}

int main()
{
  integer<bool>("bool");
  integer<char>("char");
  integer<signed char>("signed char");
  integer<unsigned char>("unsigned char");
  integer<short>("short");
  integer<unsigned short>("unsigned short");
  integer<int>("int");
  integer<unsigned int>("unsigned int");
  integer<long>("long");
  integer<long long>("long long");
  integer<wchar_t>("wchar_t");
  integer<char16_t>("char16_t");
  integer<char32_t>("char32_t");
  unsignedInteger<unsigned long>("unsigned long");
  unsignedInteger<unsigned long long>("unsigned long long");
  unsignedInteger<char32_t>("char32_t");
  floating<float>("float");
  floating<double>("double");
  return 0;
}
