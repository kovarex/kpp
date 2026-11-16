#include <cstdio>
#include <cstdint>
#include <chrono>
#include <ranges>
#include <string>
#include <vector>
template <typename T> using Vector = std::vector<T>;
template <typename It> using Subrange = std::ranges::subrange<It>;
using String = std::string;
template <typename A, typename B> auto iotaView(A a, B b) { return std::views::iota(a, (A)b); }
#define transformView std::views::transform
#define reverseView std::views::reverse
struct CivilTime
{
  int year = 0, month = 0, day = 0, weekday = 0;
};
inline long long daysFromCivil(long long y, int m, int d)
{
  using namespace std::chrono;
  return sys_days(year_month_day{year((int)y), month((unsigned)m), day((unsigned)d)}).time_since_epoch().count();
}
inline void civilFromDays(long long n, CivilTime& out)
{
  using namespace std::chrono;
  sys_days s{days{n}};
  year_month_day ymd{s};
  out.year = (int)ymd.year();
  out.month = (int)(unsigned)ymd.month();
  out.day = (int)(unsigned)ymd.day();
  out.weekday = (int)weekday{s}.c_encoding();
}
inline bool isLeapYear(long long y) { return std::chrono::year((int)y).is_leap(); }
inline int daysInMonth(long long y, int m)
{
  using namespace std::chrono;
  return (int)(unsigned)year_month_day_last{year((int)y), month_day_last{month((unsigned)m)}}.day();
}
// Views against std::views, the calendar arithmetic against <chrono>'s year_month_day.
int calls = 0;
int square(int x) { calls++; return x * x; }
template <typename R> void printAll(const char* label, R&& r)
{
  printf("%s:", label);
  for (auto&& x : r)
    printf(" %d", (int)x);
  printf(" (calls %d)\n", calls);
  calls = 0;
}
struct Item
{
  int id;
  int weight;
};
void views()
{
  Vector<int> v = {1, 2, 3, 4, 5};
  Vector<int> none;
  printAll("iota", iotaView(0, 5));
  printAll("iota negative", iotaView(-3, 2));
  printAll("iota empty", iotaView(4, 4));
  auto io = iotaView(2, 9);
  printf("iota size %d empty %d\n", (int)io.size(), (int)io.empty());
  printf("iota distance %d third %d\n", (int)(io.end() - io.begin()), (int)*(io.begin() + 3));
  auto t = transformView(v, square);
  printAll("transform", t);
  printf("size %d empty %d calls %d\n", (int)t.size(), (int)t.empty(), calls);
  int front = t.front(), back = t.back();
  printf("front %d back %d calls %d\n", front, back, calls);
  calls = 0;
  printAll("piped", v | transformView(square));
  printAll("reverse", reverseView(v));
  printAll("reverse of transform", reverseView(t));
  auto rt = reverseView(v);
  printAll("transform of reverse", transformView(rt, square));
  auto twice = transformView(t, [](int x) { return x + 1; });
  printAll("transform of transform", twice);
  printAll("of iota", iotaView(1, 4) | transformView(square));
  auto ri = iotaView(1, 4);
  printAll("reverse of iota", reverseView(ri));
  printAll("empty transform", transformView(none, square));
  printAll("empty reverse", reverseView(none));
  printf("empty sizes %d %d\n", (int)transformView(none, square).empty(), (int)reverseView(none).empty());
  // The view reads the container as it is when read.
  auto late = transformView(v, square);
  v[0] = 10;
  printAll("late", late);
  v.push_back(6);
  printAll("grown", late);
  printAll("grown reverse", reverseView(v));
  v[0] = 1;
  // Elements by reference: written through the view.
  Vector<Item> items = {{1, 10}, {2, 20}, {3, 30}};
  auto weights = transformView(items, [](Item& i) -> int& { return i.weight; });
  for (int& w : weights)
    w += 1;
  weights.front() = 100;
  for (int& w : reverseView(items | transformView([](Item& i) -> int& { return i.id; })))
    w *= 2;
  for (const Item& i : items)
    printf(" %d/%d", i.id, i.weight);
  printf("\n");
  for (int& x : reverseView(v))
    x += 1;
  printAll("written", v);
  // Subrange
  Subrange<Vector<int>::iterator> middle(v.begin() + 1, v.begin() + 4);
  printf("subrange size %d empty %d\n", (int)middle.size(), (int)middle.empty());
  printAll("subrange", middle);
  printAll("subrange reverse", reverseView(middle));
  printAll("subrange transform", transformView(middle, square));
  *middle.begin() = 50;
  printAll("through subrange", v);
  Subrange<Vector<int>::iterator> nothing(v.begin(), v.begin());
  printf("nothing %d %d\n", (int)nothing.size(), (int)nothing.empty());
  // Iterators: comparison, decrement, difference through a transform.
  auto a = t.begin(), b = t.end();
  --b;
  printf("last %d, a != b %d, a == a %d, distance %d\n", (int)*b, (int)(a != b), (int)(a == a), (int)(b - a));
  calls = 0;
  // Stateful function, called each read.
  int counter = 0;
  auto stateful = transformView(v, [&counter](int x) { return x + counter++; });
  printAll("first read", stateful);
  printAll("second read", stateful);
  printf("counter %d\n", counter);
  // Copies of a view, and the type of an element.
  auto copy = t;
  printAll("copy", copy);
  auto doubles = transformView(v, [](int x) { return x / 2.0; });
  for (double d : doubles)
    printf(" %g", d);
  printf("\n");
  auto texts = transformView(v, [](int x) { return String(x > 3 ? "big" : "small"); });
  for (const String& s : texts)
    printf(" %s", s.c_str());
  printf("\n");
}
void date(long long y, int m, int d)
{
  long long days = daysFromCivil(y, m, d);
  CivilTime c;
  civilFromDays(days, c);
  printf("%lld-%d-%d = %lld -> %lld-%d-%d wd %d\n", y, m, d, days, (long long)c.year, (int)c.month, (int)c.day, (int)c.weekday);
}
void calendar()
{
  date(1970, 1, 1);
  date(1969, 12, 31);
  date(2000, 2, 29);
  date(1900, 2, 28);
  date(1900, 3, 1);
  date(2024, 2, 29);
  date(2100, 3, 1);
  date(1, 1, 1);
  date(0, 1, 1);
  date(0, 2, 29);
  date(0, 12, 31);
  date(-1, 12, 31);
  date(-1, 1, 1);
  date(-4, 2, 29);
  date(-100, 3, 1);
  date(-400, 2, 29);
  date(-401, 3, 1);
  date(1582, 10, 15);
  date(1582, 10, 4);
  date(9999, 12, 31);
  date(29000, 12, 31);
  date(-29000, 1, 1);
  for (long long y : {-29000LL, -2000LL, -401LL, -400LL, -100LL, -5LL, -4LL, -1LL, 0LL, 1LL, 4LL, 100LL, 400LL, 1900LL, 2000LL, 2023LL, 2024LL, 29000LL})
  {
    printf("%lld leap %d:", y, (int)isLeapYear(y));
    for (int m = 1; m <= 12; m++)
      printf(" %d", (int)daysInMonth(y, m));
    printf("\n");
  }
  // Round trip and agreement over every day of a wide range, with a checksum to compare.
  unsigned long long sum = 0;
  int bad = 0;
  for (long long days = -11000000; days <= 11000000; days += 1)
  {
    CivilTime c;
    civilFromDays(days, c);
    if (daysFromCivil(c.year, c.month, c.day) != days)
      bad++;
    sum = sum * 1000003ull + (unsigned long long)(c.year * 10000 + c.month * 100 + c.day) + (unsigned long long)c.weekday;
  }
  printf("round trip bad %d sum %llu\n", bad, sum);
}
int main()
{
  views();
  calendar();
  return 0;
}
