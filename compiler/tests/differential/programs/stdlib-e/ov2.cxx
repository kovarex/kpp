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
int made = 0, copied = 0, moved = 0, copyAssigned = 0, moveAssigned = 0, destroyed = 0;
struct Tracked
{
  int v;
  Tracked() : v(0) { ++made; }
  Tracked(int x) : v(x) { ++made; }
  Tracked(const Tracked& o) : v(o.v) { ++copied; }
  Tracked(Tracked&& o) : v(o.v) { o.v = -1; ++moved; }
  Tracked& operator=(const Tracked& o) { v = o.v; ++copyAssigned; return *this; }
  Tracked& operator=(Tracked&& o) { v = o.v; o.v = -1; ++moveAssigned; return *this; }
  ~Tracked() { ++destroyed; }
  bool operator==(const Tracked& o) const { return v == o.v; }
  bool operator<(const Tracked& o) const { return v < o.v; }
  bool operator>(const Tracked& o) const { return v > o.v; }
  bool operator<=(const Tracked& o) const { return v <= o.v; }
  bool operator>=(const Tracked& o) const { return v >= o.v; }
  bool operator!=(const Tracked& o) const { return v != o.v; }
};
int throwersAlive = 0;
struct Thrower
{
  int v;
  IMPLICIT Thrower(int x) : v(x)
  {
    if (x < 0)
      throw 7;
    ++throwersAlive;
  }
  IMPLICIT Thrower(const Thrower& o) : v(o.v)
  {
    if (o.v == 13)
      throw 13;
    ++throwersAlive;
  }
  IMPLICIT Thrower(Thrower&& o) : v(o.v) { ++throwersAlive; }
  Thrower& operator=(const Thrower& o) { v = o.v; return *this; }
  Thrower& operator=(Thrower&& o) { v = o.v; return *this; }
  ~Thrower() { --throwersAlive; }
  bool operator==(const Thrower& o) const { return v == o.v; }
  bool operator<(const Thrower& o) const { return v < o.v; }
  bool operator>(const Thrower& o) const { return v > o.v; }
  bool operator<=(const Thrower& o) const { return v <= o.v; }
  bool operator>=(const Thrower& o) const { return v >= o.v; }
  bool operator!=(const Thrower& o) const { return v != o.v; }
};
void counts(const char* step)
{
  printf("%-30s made %d copied %d moved %d copy= %d move= %d destroyed %d\n", step, made, copied, moved, copyAssigned, moveAssigned, destroyed);
  made = copied = moved = copyAssigned = moveAssigned = destroyed = 0;
}
#include <variant>
using std::move;
using U = std::variant<Tracked, int, std::string>;
using T = std::variant<int, Thrower>;
void show(const char* name, const U& u)
{
  switch (u.index())
  {
    case 0: printf("  %s: Tracked %d\n", name, std::get<0>(u).v); break;
    case 1: printf("  %s: int %d\n", name, std::get<1>(u)); break;
    case 2: printf("  %s: String [%s]\n", name, std::get<2>(u).c_str()); break;
    default: printf("  %s: valueless\n", name);
  }
}
void showThrower(const char* name, const T& t)
{
  switch (t.index())
  {
    case 0: printf("  %s: int %d\n", name, std::get<0>(t)); break;
    case 1: printf("  %s: Thrower %d\n", name, std::get<1>(t).v); break;
    default: printf("  %s: valueless\n", name);
  }
}
void throwInPlace(T& t) { t.emplace<1>(-1); }
int main()
{
  {
    U a;
    counts("default");
    show("a", a);
    a = 5;
    counts("= int over Tracked");
    show("a", a);
    a = Tracked(3);
    counts("= Tracked temporary over int");
    a = Tracked(4);
    counts("= Tracked temporary over same");
    Tracked t(6);
    counts("a Tracked");
    a = t;
    counts("= Tracked lvalue over same");
    show("a", a);
    U b = a;
    counts("copy");
    U c = move(a);
    counts("move");
    show("moved-from a", a);
    show("c", c);
    b = c;
    counts("copy= same alternative");
    U d = 2;
    d = c;
    counts("copy= Tracked over int");
    c = U(String("text"));
    counts("= String union over Tracked");
    show("c", c);
    c = move(d);
    counts("move= Tracked over String");
    show("c", c);
    U e = String("a string long enough not to be stored in the object itself");
    U f = e;
    show("f", f);
    printf("f == e %d\n", (int)(f == e));
  }
  counts("scope end");
  U one = 1, two = 2, text = String("b"), earlier = String("a"), tracked = Tracked(9);
  printf("== %d %d %d, != %d\n", (int)(one == one), (int)(one == two), (int)(one == text), (int)(one != text));
  printf("< %d %d %d %d %d, > %d, <= %d, >= %d\n", (int)(one < two), (int)(two < one), (int)(tracked < one), (int)(one < text), (int)(earlier < text), (int)(text > one),
         (int)(text <= earlier), (int)(tracked >= tracked));
  counts("comparisons");
  {
    T x = Thrower(1);
    T y = Thrower(13);
    printf("throwers %d\n", throwersAlive);
    try
    {
      x = y;
      printf("copy= did not throw\n");
    }
    catch (int code)
    {
      printf("copy= threw %d\n", code);
    }
    showThrower("x after a throwing copy=", x);
    printf("throwers %d\n", throwersAlive);
    T z = 2;
    try
    {
      throwInPlace(z);
      printf("in place did not throw\n");
    }
    catch (int code)
    {
      printf("in place threw %d\n", code);
    }
    showThrower("z after a throwing construction in place", z);
    printf("throwers %d\n", throwersAlive);
  }
  printf("throwers at the end %d\n", throwersAlive);
  return 0;
}
