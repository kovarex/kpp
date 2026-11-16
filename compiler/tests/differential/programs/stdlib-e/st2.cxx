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
#include <string_view>
#include <map>
#include <cstring>
#include <compare>
using StringView = std::string_view;
template <typename K, typename V> using Map = std::map<K, V>;
using std::sort;
// String and StringView, the parts st1 left out: ordering of bytes above 0x7F (std::char_traits<char>
// compares as unsigned char) through <, <=>, compare, ==, sorting and a Map's order; embedded NULs in
// lengths, comparisons and finds; reserve, capacity, shrink_to_fit and clear; starts_with and
// ends_with with chars, views and empty arguments; data() staying put without growth.
int sign(int c) { return c < 0 ? -1 : c > 0 ? 1 : 0; }
int main()
{
  const char* texts[] = {"", "a", "A", "\x7f", "\x80", "\xff", "\xc3\xa9", "a\x80", "a\x7f", "ab", "a\xff" "b", "\xe2\x82\xac"};
  int n = 12;
  for (int i = 0; i < n; ++i)
  {
    printf("%2d:", i);
    for (int j = 0; j < n; ++j)
    {
      String a(texts[i]), b(texts[j]);
      StringView va(a.data(), a.size()), vb(b.data(), b.size());
      int c = sign(a.compare(b));
      int v = sign(va.compare(vb));
      int lt = a < b;
      int three = (a <=> b) < 0 ? -1 : (a <=> b) > 0 ? 1 : 0;
      int vlt = va < vb;
      printf(" %d%d%d%d%d", c + 1, v + 1, lt, three + 1, vlt);
    }
    printf("\n");
  }
  Vector<String> sorted;
  for (int i = 0; i < n; ++i)
    sorted.push_back(String(texts[i]));
  sort(sorted.begin(), sorted.end());
  printf("sorted:");
  for (const String& s : sorted)
  {
    printf(" [");
    for (char ch : s)
      printf("%02x", (unsigned char)ch);
    printf("]");
  }
  printf("\n");
  Map<String, int> ordered;
  for (int i = 0; i < n; ++i)
    ordered[String(texts[i])] = i;
  printf("map:");
  for (const auto& e : ordered)
    printf(" %d", e.second);
  printf("\n");

  String nul("a\0b\0c", 5);
  String nul2("a\0b", 3);
  StringView view(nul.data(), nul.size());
  printf("NUL: size %d, find %d %d, rfind %d, compare %d, == %d, starts %d, ends %d, substr size %d, view find %d\n", (int)nul.size(),
         (int)nul.find('\0'), (int)nul.find(String("b\0c", 3)), (int)nul.rfind('\0'), sign(nul.compare(nul2)), (int)(nul == nul2),
         (int)nul.starts_with(StringView(nul2.data(), nul2.size())), (int)nul.ends_with(StringView("\0c", 2)), (int)nul.substr(1, 3).size(),
         (int)view.find(StringView("\0", 1), 2));
  String appended = nul;
  appended.append(nul);
  printf("NUL append size %d, last %d, c_str length %d\n", (int)appended.size(), (int)appended.back(), (int)strlen(appended.c_str()));

  String s;
  printf("empty capacity >= 0, size %d, c_str empty %d\n", (int)s.size(), (int)(s.c_str()[0] == 0));
  s.reserve(100);
  bool atLeast = s.capacity() >= 100;
  const char* block = s.data();
  for (int i = 0; i < 100; ++i)
    s.push_back('x');
  printf("reserve 100: at least %d, 100 pushes stay in place %d\n", (int)atLeast, (int)(s.data() == block));
  size_t before = s.capacity();
  s.reserve(10);
  printf("reserve smaller keeps the capacity %d (C++20)\n", (int)(s.capacity() == before));
  s.resize(3);
  printf("shrink_to_fit: size %d, capacity below 100 %d, text [%s]\n", (int)s.size(), (int)(s.capacity() < 100), s.c_str());
  String big(1000, 'y');
  size_t bigCapacity = big.capacity();
  big.clear();
  printf("clear keeps the capacity %d, size %d\n", (int)(big.capacity() == bigCapacity), (int)big.size());
  String grown;
  size_t lastCapacity = grown.capacity();
  int growths = 0;
  bool geometric = true;
  for (int i = 0; i < 100000; ++i)
  {
    grown.push_back('z');
    if (grown.capacity() != lastCapacity)
    {
      if (lastCapacity > 64 && grown.capacity() < lastCapacity + lastCapacity / 3)
        geometric = false;
      lastCapacity = grown.capacity();
      ++growths;
    }
  }
  printf("100000 pushes: growth geometric %d, fewer than 60 growths %d\n", (int)geometric, (int)(growths < 60));

  String t("hello world");
  printf("starts/ends: %d %d %d %d %d %d %d %d\n", (int)t.starts_with('h'), (int)t.starts_with(StringView("hello")), (int)t.starts_with(StringView("")),
         (int)t.ends_with('d'), (int)t.ends_with(StringView("world")), (int)t.ends_with(StringView("")), (int)String().starts_with('a'),
         (int)String().ends_with(StringView("")));
  return 0;
}
