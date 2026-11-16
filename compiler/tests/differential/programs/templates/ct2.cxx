#include <stdio.h>
#include <compare>
#include <compare>
#include <string.h>
#include <stdint.h>
#include <initializer_list>
#include <new>
#include <tuple>
#include <utility>
template <typename... T> using Tuple = std::tuple<T...>;
template <typename A, typename B> using Pair = std::pair<A, B>;
using std::get;
using std::tie;
// Tuples of references, tie and shadowing at compile time.
constexpr int prvalueOfReferences()
{
  int a = 0, b = 0;
  Tuple<int&, int&>(a, b) = Pair<int, int>(1, 2);
  return a * 10 + b;
}
constexpr int assignedFromATuple()
{
  int a = 0, b = 0;
  Tuple<int&, int&> t(a, b);
  t = Tuple<int, int>(3, 4);
  return a * 10 + b;
}
constexpr int throughGet()
{
  int a = 1, b = 2;
  Tuple<int&, int&> t(a, b);
  get<0>(t) = 7;
  get<1>(t) += 5;
  return a * 10 + b;
}
constexpr int tieFromTie()
{
  int a = 1, b = 2;
  tie(a, b) = tie(b, a);
  return a * 10 + b;
}
constexpr int structuredBinding()
{
  int a = 1, b = 2;
  auto [x, y] = tie(a, b);
  x = 5;
  y = 6;
  return a * 10 + b;
}
constexpr int copyOfReferences()
{
  int a = 1, b = 2;
  Tuple<int&, int&> t(a, b);
  auto u = t;
  get<0>(u) = 9;
  return a * 10 + b;
}
constexpr int mixed()
{
  int a = 1;
  Tuple<int&, int> t(a, 2);
  get<0>(t) = 3;
  get<1>(t) = 4;
  return a * 10 + get<1>(t);
}
constexpr int constReference()
{
  int a = 7;
  Tuple<const int&, char> t(a, 'x');
  return get<0>(t) + get<1>(t);
}
constexpr int rvalueReference()
{
  int a = 7;
  Tuple<int&&> t(static_cast<int&&>(a));
  get<0>(t) = 8;
  return a;
}
constexpr long t2 = 5;
constexpr int shadowed() { Tuple<int, char> t2(3, 'a'); return get<0>(t2); }
constexpr int shadowedInABlock()
{
  int n = 0;
  {
    Tuple<int, char> t2(4, 'b');
    n = get<0>(t2);
  }
  return n + int(t2);
}
constexpr int shadowedByAParameter(Tuple<int, char> t2) { return get<0>(t2) + 1; }
constexpr int twiceOf(long v) { return int(v * 2); }
template <typename T> constexpr auto firstOf(const T& v) { return get<0>(v); }
constexpr int shadowedThroughATemplate() { Tuple<int, char> t2(6, 'c'); return firstOf(t2); }
struct Holder { long t2 = 11; constexpr int local() const { Tuple<int, char> t2(7, 'd'); return get<0>(t2) + int(this->t2); } };
constexpr int theGlobalBesideALocal() { Tuple<int, char> local(1, 'e'); return get<0>(local) + int(t2); }
constexpr int lambdaShadow() { Tuple<int, char> t2(8, 'f'); auto l = [&] { return get<0>(t2); }; return l(); }
constexpr int get0 = 3;
constexpr int shadowedName() { long get0 = 4; return int(get0) + 1; }
template <int N> constexpr int nth() { return N; }
constexpr int nthShadow() { int N = 3; return nth<2>() + N; }
constexpr int c1 = prvalueOfReferences();
constexpr int c2 = assignedFromATuple();
constexpr int c3 = throughGet();
constexpr int c4 = tieFromTie();
constexpr int c5 = structuredBinding();
constexpr int c6 = copyOfReferences();
constexpr int c7 = mixed();
constexpr int c8 = constReference();
constexpr int c9 = rvalueReference();
constexpr int c10 = shadowed();
constexpr int c11 = shadowedInABlock();
constexpr int c12 = shadowedByAParameter(Tuple<int, char>(1, 'x'));
constexpr int c13 = shadowedThroughATemplate();
constexpr int c14 = Holder().local();
constexpr int c15 = theGlobalBesideALocal();
constexpr int c16 = lambdaShadow();
constexpr int c17 = shadowedName();
constexpr int c18 = nthShadow();
int main()
{
  printf("%d %d %d %d %d %d\n", c1, c2, c3, c4, c5, c6);
  printf("%d %d %d %d %d %d\n", c7, c8, c9, c10, c11, c12);
  printf("%d %d %d %d %d %d\n", c13, c14, c15, c16, c17, c18);
  printf("%d %d %d %d %d %d\n", prvalueOfReferences(), assignedFromATuple(), throughGet(), tieFromTie(), structuredBinding(), copyOfReferences());
  printf("%d %d %d %d %d\n", mixed(), constReference(), rvalueReference(), shadowed(), shadowedInABlock());
  return 0;
}
