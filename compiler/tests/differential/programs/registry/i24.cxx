// The kpp stdlib names over the C++ standard library, so one battery body compiles as both.
#include <stdio.h>
#include <vector>
#include <deque>
#include <list>
#include <map>
#include <set>
#include <unordered_map>
#include <string>
#include <algorithm>
#include <numeric>
#include <utility>
template <class T> using Vector = std::vector<T>;
template <class T> using Deque = std::deque<T>;
template <class T> using List = std::list<T>;
template <class K, class V, class C = std::less<K>> using Map = std::map<K, V, C>;
template <class K, class V> using MultiMap = std::multimap<K, V>;
template <class T> using Set = std::set<T>;
template <class T> using MultiSet = std::multiset<T>;
template <class K, class V> using HashMap = std::unordered_map<K, V>;
template <class A, class B> using Pair = std::pair<A, B>;
using String = std::string;
#define WRAP(kpp, std_) template <class... A> auto kpp(A&&... a) { return std::std_(std::forward<A>(a)...); }
WRAP(sort, sort) WRAP(stableSort, stable_sort) WRAP(nthElement, nth_element) WRAP(unique, unique) WRAP(remove, remove)
WRAP(removeIf, remove_if) WRAP(rotate, rotate) WRAP(nextPermutation, next_permutation) WRAP(lowerBound, lower_bound)
WRAP(upperBound, upper_bound) WRAP(equalRange, equal_range) WRAP(binarySearch, binary_search) WRAP(merge, merge)
WRAP(setDifference, set_difference) WRAP(setIntersection, set_intersection) WRAP(lexicographicalCompare, lexicographical_compare)
WRAP(accumulate, accumulate) WRAP(minElement, min_element) WRAP(maxElement, max_element) WRAP(copyBackward, copy_backward)
WRAP(reverse, reverse) WRAP(find, find) WRAP(findIf, find_if) WRAP(count, count) WRAP(countIf, count_if) WRAP(equal, equal)
WRAP(adjacentFind, adjacent_find) WRAP(fill, fill) WRAP(transform, transform) WRAP(replace, replace) WRAP(copy, copy)
WRAP(isSorted, is_sorted) WRAP(isSortedUntil, is_sorted_until) WRAP(copyIf, copy_if) WRAP(distance, distance)
template <class C, class P> auto eraseIf(C& c, P p) { return std::erase_if(c, p); }
// More kpp stdlib names over the C++ standard library (battery i10).
#include <optional>
#include <tuple>
#include <queue>
#include <string_view>
#include <functional>
template <class T> using Optional = std::optional<T>;
template <class... T> using Tuple = std::tuple<T...>;
template <class T, class C = std::vector<T>, class L = std::less<T>> using PriorityQueue = std::priority_queue<T, C, L>;
using StringView = std::string_view;
template <class T> using Greater = std::greater<T>;
using std::get;
template <class... A> auto makeTuple(A&&... a) { return std::make_tuple(std::forward<A>(a)...); }
template <class... A> auto tie(A&... a) { return std::tie(a...); }
WRAP(clamp, clamp)
template <class K, class V, class C> using MapC = std::map<K, V, C>;
using std::move;
// battery i24: ordered containers further: a comparator (greater) ordering a Map and a Set,
// MultiMap's equal_range and count, insert with a hint, merge of two maps (equal keys stay in the
// source), extract and reinsert of a node with a new key, erase of a range, Set's lower and upper
// bounds at both ends, emplace returning the existing element, operator[] default-constructing.
int main()
{
  Map<int, int, Greater<int>> desc;
  for (int i = 0; i < 6; ++i)
    desc[i * 3 % 7] = i;
  printf("desc");
  for (auto& e : desc)
    printf(" %d:%d", e.first, e.second);
  printf(" | first %d lb %d\n", desc.begin()->first, desc.lower_bound(4)->first);
  MultiMap<int, char> mm;
  const char* letters = "abcdefg";
  for (int i = 0; i < 7; ++i)
    mm.insert(Pair<int, char>(i % 3, letters[i]));
  auto range = mm.equal_range(1);
  printf("equal");
  for (auto it = range.first; it != range.second; ++it)
    printf(" %c", it->second);
  printf(" | count %d %d\n", (int)mm.count(0), (int)mm.count(5));
  Map<int, int> a = {{1, 10}, {3, 30}, {5, 50}}, b = {{3, 300}, {4, 400}};
  auto hint = a.insert(a.end(), Pair<int, int>(7, 70));
  printf("hint %d %d\n", hint->first, (int)a.size());
  a.merge(b);
  printf("merge");
  for (auto& e : a)
    printf(" %d:%d", e.first, e.second);
  printf(" | left");
  for (auto& e : b)
    printf(" %d:%d", e.first, e.second);
  printf("\n");
  auto node = a.extract(5);
  node.key() = 6;
  a.insert(move(node));
  a.erase(a.find(3), a.find(6));
  printf("extract");
  for (auto& e : a)
    printf(" %d:%d", e.first, e.second);
  printf("\n");
  Set<int> s = {10, 20, 30};
  printf("bounds %d %d %d %d\n", *s.lower_bound(5), *s.upper_bound(10), (int)(s.lower_bound(31) == s.end()), *s.lower_bound(30));
  auto em = a.emplace(1, 999);
  int& fresh = a[100];
  printf("emplace %d %d %d %d\n", (int)em.second, em.first->second, fresh, (int)a.size());
  return 0;
}
