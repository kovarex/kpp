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
#include <deque>
#include <list>
#include <span>
#include <array>
#include <iterator>
#include <compare>
template <typename T> using Deque = std::deque<T>;
template <typename T> using List = std::list<T>;
template <typename T, std::size_t N = std::dynamic_extent> using Span = std::span<T, N>;
template <typename T, std::size_t N> using Array = std::array<T, N>;
constexpr auto dynamicExtent = std::dynamic_extent;
using std::next; using std::distance; using std::swap;
// Deque, List, Span and Array against std::deque, list, span and array: insert and erase in the
// middle and the positions they return, Deque references kept across pushes at both ends and
// lost to a middle insert's moves, List splice (whole, one, into itself), merge and sort stability,
// unique, reverse, Span subspan, first and last at the edges (dynamicExtent counts too), Array's
// comparisons, fill and swap.
struct Keyed
{
  int key;
  int order;
  bool operator<(const Keyed& o) const { return key < o.key; }
  bool operator==(const Keyed& o) const { return key == o.key; }
};
template <typename C> void show(const char* label, const C& c)
{
  printf("%s:", label);
  for (const auto& x : c)
    printf(" %d", (int)x);
  printf("\n");
}
void showKeyed(const char* label, const List<Keyed>& c)
{
  printf("%s:", label);
  for (const Keyed& x : c)
    printf(" %d.%d", x.key, x.order);
  printf("\n");
}

int main()
{
  Deque<int> d;
  for (int i = 0; i < 20; ++i)
    d.push_back(i);
  auto it = d.insert(d.begin() + 7, 100);
  printf("deque insert returns %d -> %d\n", (int)(it - d.begin()), *it);
  it = d.insert(d.begin() + 15, 3, 200);
  printf("deque insert 3 returns %d -> %d\n", (int)(it - d.begin()), *it);
  it = d.insert(d.begin() + 2, 9);
  it = d.insert(it, 8);
  it = d.insert(it, 7);
  printf("deque insert three returns %d -> %d, size %d\n", (int)(it - d.begin()), *it, (int)d.size());
  it = d.erase(d.begin() + 5);
  printf("deque erase returns %d -> %d\n", (int)(it - d.begin()), *it);
  it = d.erase(d.begin() + 1, d.begin() + 4);
  printf("deque erase range returns %d -> %d\n", (int)(it - d.begin()), *it);
  it = d.erase(d.end() - 1);
  printf("deque erase last returns end %d\n", (int)(it == d.end()));
  it = d.erase(d.begin());
  printf("deque erase first returns %d -> %d\n", (int)(it - d.begin()), *it);
  show("deque", d);
  int& front = d.front();
  int& back = d.back();
  int& middle = d[10];
  for (int i = 0; i < 5000; ++i)
  {
    d.push_front(-i);
    d.push_back(1000 + i);
  }
  printf("deque references after 10000 pushes at the ends: %d %d %d\n", (int)(&front == &d[5000]), (int)(&back == &d[d.size() - 5001]),
         (int)(&middle == &d[5010]));
  for (int i = 0; i < 4000; ++i)
  {
    d.pop_front();
    d.pop_back();
  }
  printf("deque references after 8000 pops at the ends: %d %d %d, size %d\n", (int)(&front == &d[1000]), (int)(&back == &d[d.size() - 1001]),
         (int)(&middle == &d[1010]), (int)d.size());
  d.resize(5);
  show("deque resized to 5", d);

  List<int> l;
  for (int x : {5, 1, 4, 1, 5, 9, 2, 6})
    l.push_back(x);
  auto li = l.insert(next(l.begin(), 3), 77);
  printf("list insert returns %d at %d\n", *li, (int)distance(l.begin(), li));
  li = l.insert(l.end(), 88);
  l.push_back(88);
  printf("list insert at the end returns %d at %d\n", *li, (int)distance(l.begin(), li));
  li = l.erase(next(l.begin(), 2), next(l.begin(), 5));
  printf("list erase range returns %d at %d\n", *li, (int)distance(l.begin(), li));
  show("list", l);
  List<int> other;
  for (int x : {100, 101, 102})
    other.push_back(x);
  int* kept = &other.front();
  l.splice(next(l.begin()), other);
  printf("splice all: other empty %d, element kept %d at %d\n", (int)other.empty(), (int)(kept == &*next(l.begin())), (int)l.size());
  other.push_back(300);
  other.push_back(301);
  l.splice(l.begin(), other, next(other.begin()));
  printf("splice one: first %d, other size %d\n", l.front(), (int)other.size());
  l.splice(l.end(), l, l.begin());
  printf("splice within itself: first %d last %d\n", l.front(), l.back());
  show("list", l);
  l.sort();
  show("sorted", l);
  l.unique();
  show("unique", l);
  l.reverse();
  show("reversed", l);
  l.sort([](int a, int b) { return a > b; });
  show("sorted descending", l);

  List<Keyed> a, b;
  for (int i = 0; i < 8; ++i)
    a.push_back(Keyed{(i * 3) % 4, i});
  for (int i = 0; i < 6; ++i)
    b.push_back(Keyed{(i * 5) % 3, 10 + i});
  a.sort();
  b.sort();
  showKeyed("stable sort a", a);
  showKeyed("stable sort b", b);
  a.merge(b);
  showKeyed("merge keeps a's first on ties", a);
  a.unique();
  showKeyed("unique by key", a);

  int raw[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  Span<int> s(raw, 10);
  show("subspan(3, 4)", s.subspan(3, 4));
  show("subspan(7)", s.subspan(7));
  show("subspan(10)", s.subspan(10));
  show("first(0)", s.first(0));
  show("first(10)", s.first(10));
  show("last(3)", s.last(3));
  show("last(0)", s.last(0));
  Span<int, 4> fixed(raw + 2, 4);
  printf("fixed extent %d, size %d, size_bytes %d, front %d back %d\n", (int)fixed.extent, (int)fixed.size(), (int)fixed.size_bytes(), fixed.front(),
         fixed.back());
  Span<const int> view = s;
  printf("const view of all: %d, empty span %d\n", (int)view.size(), (int)Span<int>().empty());

  Array<int, 4> x = {1, 2, 3, 4}, y = {1, 2, 4, 0}, z = {1, 2, 3, 4};
  printf("array: == %d %d, < %d %d, <= %d, > %d\n", (int)(x == z), (int)(x == y), (int)(x < y), (int)(y < x), (int)(x <= z), (int)(y > x));
  x.fill(7);
  show("fill 7", x);
  swap(x, y);
  show("swapped x", x);
  show("swapped y", y);
  Array<int, 0> none;
  printf("empty array: size %d, empty %d, begin == end %d\n", (int)none.size(), (int)none.empty(), (int)(none.begin() == none.end()));
  return 0;
}
