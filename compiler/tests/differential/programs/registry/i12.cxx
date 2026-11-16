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
template <class K, class V> using Map = std::map<K, V>;
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
// battery i12: the compile-time interpreter running the stdlib's Vector and String (C++20 constexpr
// std::vector and std::string on the C++ side): growth, insert and erase in the middle, sort,
// reverse, find, appends, substr, comparisons; each result at compile time beside the same
// function at run time.
constexpr int vectorWork(int n)
{
  Vector<int> v;
  for (int i = 0; i < n; ++i)
    v.push_back((i * 7) % 11);
  v.insert(v.begin() + 2, 99);
  v.erase(v.begin() + 5);
  sort(v.begin(), v.end());
  reverse(v.begin(), v.begin() + 3);
  int s = 0;
  for (int x : v)
    s = s * 3 + x;
  return s % 1000003 + (int)v.size() * 1000000;
}
constexpr int stringWork()
{
  String a = "kpp";
  String b = a;
  b.append(" compiles");
  b.push_back('!');
  String c = b.substr(4, 8);
  int s = (int)b.size() * 100 + (int)c.size();
  s = s * 10 + (int)(b.find("pile") != String::npos);
  s = s * 10 + (int)(a < b);
  s = s * 10 + (int)(c == "compiles");
  b.erase(0, 4);
  return s * 1000 + (int)b[0];
}
constexpr int nested()
{
  Vector<Vector<int>> grid(3);
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c <= r; ++c)
      grid[r].push_back(r * 10 + c);
  Vector<Vector<int>> copy = grid;
  copy[1].push_back(5);
  int s = 0;
  for (const auto& row : copy)
    s = s * 10 + (int)row.size();
  return s * 100 + copy[2][2] + (int)grid[1].size();
}
constexpr int cv1 = vectorWork(20), cs1 = stringWork(), cn1 = nested();
int main(int argc, char**)
{
  int z = argc - 1;
  printf("vector %d %d\n", cv1, vectorWork(20 + z));
  printf("string %d %d\n", cs1, stringWork());
  printf("nested %d %d\n", cn1, nested());
  return 0;
}
