#include <stdio.h>
#include <stdint.h>
#include <type_traits>

enum Level : uint32_t { Low = 1, High = 2 };

struct S
{
  unsigned u3 : 3;
  unsigned u32 : 32;
  int s5 : 5;
  uint64_t w3 : 3;
  uint64_t w32 : 32;
  uint64_t w40 : 40;
  int64_t i20 : 20;
  Level level : 3;
  bool on : 1;
  int64_t fromMember() const { return u3 - 2; }
};

constexpr int64_t viaConstant()
{
  S t{};
  t.u3 = 1;
  t.w3 = 2;
  return (t.u3 - 2) * 10 + (t.w3 - 3 < 0 ? 1 : 0);
}
static_assert(viaConstant() == -9);

int main()
{
  S s{};
  s.u3 = 1;
  s.u32 = 1;
  s.s5 = -3;
  s.w3 = 2;
  s.w32 = 5;
  s.w40 = 7;
  s.i20 = 4;
  s.level = Low;
  s.on = true;
  bool no = s.u32 == 0;
  // promoted to int: an unsigned narrower than 32 bits, a 64-bit one narrower than 32 bits
  int64_t a = s.u3 - 2;
  int64_t b = -s.u3;
  int64_t c = ~s.u3;
  int64_t d = s.u3 << 31;
  int64_t e = (s.w3 - 3) / 2;
  int64_t f = no ? s.u3 : -1;
  printf("%lld %lld %lld %lld %lld %lld\n", (long long)a, (long long)b, (long long)c, (long long)d, (long long)e, (long long)f);
  printf("%d %d %d %d\n", s.u3 - 2 < 0, s.w3 - 3 < 0, s.s5 < s.u3, (s.u3 = 1) - 2 < 0);
  // a 32-bit unsigned one an unsigned int, a 64-bit one of 32 bits too; a wider one keeps its type
  int64_t g = s.u32 - 2;
  int64_t h = s.w32 - 6;
  printf("%lld %lld %d\n", (long long)g, (long long)h, s.w40 - 8 < 0);
  // an enum bit-field promotes as its enum (a fixed underlying uint32_t: unsigned), a bool one as bool
  int64_t i = s.level - 2;
  int64_t j = s.on - 2;
  printf("%lld %lld\n", (long long)i, (long long)j);
  // the types: sizeof and the type of an expression
  printf("%d %d %d %d %d\n", (int)sizeof(s.i20 + 0), (int)sizeof(s.w3 + 0), (int)sizeof(s.w40 + 0), (int)std::is_same_v<decltype(s.u3 + 0), int>,
         (int)std::is_same_v<decltype(s.w32 + 0), unsigned>);
  // a compound assignment computes in the promoted type: 6 / -2 is -3, its low 3 bits 5
  s.u3 = 6;
  s.u3 /= -2;
  printf("%d %lld %lld\n", (int)s.u3, (long long)s.fromMember(), (long long)viaConstant());
  return 0;
}
