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
#include <random>
using DefaultRandomEngine = std::mt19937_64;
template <typename T> using UniformIntDistribution = std::uniform_int_distribution<T>;
// <random> against std, where the standard fixes the result: a default-constructed
// uniform_int_distribution's range (0 to the type's maximum); two default-constructed engines give
// the same sequence (default_seed); the same seed, by constructor or seed(), gives the same
// sequence; a uniform distribution is uniform, also over a range near 2^64 (no modulo bias); the
// bounds are inclusive and reached; a range of one value.
int main()
{
  UniformIntDistribution<int> d;
  printf("default distribution: a %d b %d\n", d.a(), d.b());
  UniformIntDistribution<unsigned long long> du;
  printf("default u64 distribution: b is max %d\n", (int)(du.b() == 18446744073709551615ull));
  DefaultRandomEngine e1, e2;
  int same = 0;
  for (int i = 0; i < 10; ++i)
    same += e1() == e2();
  printf("two default engines agree %d of 10\n", same);
  DefaultRandomEngine s1(42), s2(7);
  s2.seed(42);
  same = 0;
  for (int i = 0; i < 10; ++i)
    same += s1() == s2();
  printf("seed 42 by constructor and by seed() agree %d of 10\n", same);
  DefaultRandomEngine e(12345);
  int counts[6] = {0};
  UniformIntDistribution<int> die(1, 6);
  for (int i = 0; i < 600000; ++i)
  {
    int v = die(e);
    if (v < 1 || v > 6)
    {
      printf("out of range %d\n", v);
      return 1;
    }
    ++counts[v - 1];
  }
  int even = 1;
  for (int c : counts)
    even = even && c > 98000 && c < 102000;
  printf("die faces even %d\n", even);
  UniformIntDistribution<int> one(7, 7);
  printf("range of one: %d %d\n", one(e), one(e));
  UniformIntDistribution<int> negative(-3, -1);
  int seen[3] = {0};
  for (int i = 0; i < 3000; ++i)
    ++seen[negative(e) + 3];
  printf("negative range reaches every value %d\n", (int)(seen[0] && seen[1] && seen[2]));
  UniformIntDistribution<long long> full(-9223372036854775807ll - 1, 9223372036854775807ll);
  int negatives = 0;
  for (int i = 0; i < 100000; ++i)
    negatives += full(e) < 0;
  printf("full i64 range: about half negative %d\n", (int)(negatives > 49000 && negatives < 51000));
  // [0, 3 * 2^62): a third of the values are below 2^62.
  UniformIntDistribution<unsigned long long> wide(0, 3ull * 4611686018427387904ull - 1);
  int low = 0;
  for (int i = 0; i < 300000; ++i)
    low += wide(e) < 4611686018427387904ull;
  printf("range 3 * 2^62: below 2^62 a third %d (%.2f)\n", (int)(low > 98000 && low < 102000), low / 300000.0);
  return 0;
}
