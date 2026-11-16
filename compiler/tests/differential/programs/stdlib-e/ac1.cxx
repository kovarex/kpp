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
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <string_view>
#include <iterator>
using StringView = std::string_view;
using std::distance;
template <typename K, typename V, typename C = std::less<K>> using Map = std::map<K, V, C>;
template <typename K, typename V, typename C = std::less<K>> using MultiMap = std::multimap<K, V, C>;
template <typename K, typename C = std::less<K>> using Set = std::set<K, C>;
template <typename K, typename C = std::less<K>> using MultiSet = std::multiset<K, C>;
template <typename K, typename V, typename H = std::hash<K>, typename E = std::equal_to<K>> using HashMap = std::unordered_map<K, V, H, E>;
template <typename K> using HashSet = std::unordered_set<K>;
// Map, MultiMap, Set, MultiSet, HashMap and HashSet against std's: insert, operator[], try_emplace
// and insert_or_assign (which overwrite, what the result says), erase by key, by iterator and by
// range and their results, find, count, contains, lower_bound, upper_bound and equal_range below,
// between, on and past the keys, the multi containers' order of equivalent keys, a descending
// comparator, a custom hash and equality, lookups with a key of another type (a string literal,
// a StringView through a transparent comparator), iteration order, and references kept across
// inserts, erases and a rehash.
struct Descending
{
  bool operator()(int a, int b) const { return a > b; }
};
struct Transparent
{
  using is_transparent = void;
  bool operator()(const String& a, const String& b) const { return a < b; }
  bool operator()(const String& a, StringView b) const { return StringView(a.data(), a.size()) < b; }
  bool operator()(StringView a, const String& b) const { return a < StringView(b.data(), b.size()); }
};
struct ModHash
{
  size_t operator()(int x) const { return (size_t)(x % 10) * 7; }
};
struct SameModTen
{
  bool operator()(int a, int b) const { return a % 10 == b % 10; }
};
template <typename M> void bounds(const char* label, M& m, int key)
{
  auto lo = m.lower_bound(key);
  auto hi = m.upper_bound(key);
  auto range = m.equal_range(key);
  printf("%s %d: lower %d upper %d range %d..%d count %d contains %d find %d\n", label, key, lo == m.end() ? -1 : (int)lo->first,
         hi == m.end() ? -1 : (int)hi->first, range.first == m.end() ? -1 : (int)range.first->first, range.second == m.end() ? -1 : (int)range.second->first,
         (int)m.count(key), (int)m.contains(key), m.find(key) == m.end() ? -1 : (int)m.find(key)->second);
}

int main()
{
  Map<int, int> m;
  auto r = m.insert({1, 10});
  printf("insert new: %d %d\n", (int)r.second, r.first->second);
  r = m.insert({1, 11});
  printf("insert existing: %d keeps %d\n", (int)r.second, r.first->second);
  m[1] = 12;
  printf("[] overwrites: %d, [] of a new key %d, size %d\n", m[1], m[2], (int)m.size());
  r = m.try_emplace(1, 13);
  printf("try_emplace existing: %d keeps %d\n", (int)r.second, r.first->second);
  r = m.try_emplace(3, 30);
  printf("try_emplace new: %d %d\n", (int)r.second, r.first->second);
  for (int k : {5, 7, 9})
    m[k] = k * 10;
  for (int key : {0, 1, 4, 5, 9, 10})
    bounds("map", m, key);
  int e1 = (int)m.erase(5);
  int e2 = (int)m.erase(5);
  printf("erase key: %d %d, size %d\n", e1, e2, (int)m.size());
  auto it = m.erase(m.find(3));
  printf("erase iterator returns key %d\n", it->first);
  it = m.erase(m.find(7));
  printf("erase the second-to-last returns key %d\n", it->first);
  it = m.erase(m.find(9));
  printf("erase the last returns end %d\n", (int)(it == m.end()));
  it = m.erase(m.begin(), m.end());
  printf("erase all returns end %d, empty %d\n", (int)(it == m.end()), (int)m.empty());
  for (int key : {0, 5})
    bounds("empty map", m, key);

  MultiMap<int, int> mm;
  for (int i = 0; i < 8; ++i)
    mm.insert({i % 3, i});
  printf("multimap order:");
  for (const auto& e : mm)
    printf(" %d=%d", e.first, e.second);
  printf("\n");
  for (int key : {-1, 0, 1, 2, 3})
    bounds("multimap", mm, key);
  printf("multimap erase key 1: %d, size %d\n", (int)mm.erase(1), (int)mm.size());
  auto mid = mm.find(2);
  auto after = mm.erase(mid);
  printf("multimap erase the first 2 returns %d=%d\n", after->first, after->second);

  MultiSet<String> ms;
  for (const char* w : {"b", "a", "b", "c", "b", "a"})
    ms.insert(String(w));
  printf("multiset count b %d, a %d, z %d; order:", (int)ms.count(String("b")), (int)ms.count(String("a")), (int)ms.count(String("z")));
  for (const String& s : ms)
    printf(" %s", s.c_str());
  printf("\n");
  auto er = ms.equal_range(String("b"));
  printf("multiset equal_range b spans %d\n", (int)distance(er.first, er.second));

  Set<int, Descending> down;
  for (int x : {3, 1, 4, 1, 5, 9, 2, 6})
    down.insert(x);
  printf("descending set:");
  for (int x : down)
    printf(" %d", x);
  auto lb = down.lower_bound(4);
  auto ub = down.upper_bound(4);
  printf(" | lower_bound 4 -> %d, upper_bound 4 -> %d, lower_bound 0 at end %d\n", *lb, *ub, (int)(down.lower_bound(0) == down.end()));

  Map<String, int> names;
  names[String("alpha")] = 1;
  names[String("beta")] = 2;
  printf("find with a literal: %d, count %d, contains %d\n", names.find("beta")->second, (int)names.count("alpha"), (int)names.contains("gamma"));
  Map<String, int, Transparent> clear;
  clear[String("alpha")] = 1;
  clear[String("beta")] = 2;
  StringView key("beta");
  printf("transparent find with a view: %d, lower_bound %s\n", clear.find(key)->second, clear.lower_bound(StringView("b"))->first.c_str());

  HashMap<int, int> h;
  auto hr = h.insert({1, 10});
  hr = h.insert({1, 11});
  printf("hash insert existing: %d keeps %d\n", (int)hr.second, hr.first->second);
  h[1] = 12;
  hr = h.try_emplace(1, 13);
  printf("hash [] overwrites %d, try_emplace keeps %d\n", h[1], hr.first->second);
  hr = h.insert_or_assign(1, 14);
  printf("insert_or_assign existing: inserted %d value %d\n", (int)hr.second, hr.first->second);
  hr = h.insert_or_assign(2, 20);
  printf("insert_or_assign new: inserted %d value %d\n", (int)hr.second, hr.first->second);
  int* one = &h[1];
  for (int k = 3; k < 20000; ++k)
    h[k] = k;
  h.reserve(100000);
  printf("reference after 20000 inserts and reserve(100000): %d\n", *one);
  int h1 = (int)h.erase(5);
  int h2 = (int)h.erase(5);
  printf("hash erase key %d %d, count %d %d, contains %d\n", h1, h2, (int)h.count(6), (int)h.count(5), (int)h.contains(7));

  HashMap<int, int, ModHash, SameModTen> custom;
  custom[3] = 1;
  custom[13] = 2;
  custom[23] = 3;
  custom[4] = 4;
  int customSize = (int)custom.size();
  int at13 = custom[13];
  printf("custom hash and equality: size %d, [13] %d, find 33 %d, count 43 %d\n", customSize, at13, custom.find(33)->second, (int)custom.count(43));

  HashSet<String> hs;
  auto sr = hs.insert(String("x"));
  sr = hs.insert(String("x"));
  int size1 = (int)hs.size();
  int x1 = (int)hs.erase(String("x"));
  int x2 = (int)hs.erase(String("x"));
  printf("hash set insert existing %d, size %d, erase %d %d\n", (int)sr.second, size1, x1, x2);
  return 0;
}
