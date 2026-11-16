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
// battery i15: String's members at the edges of their positions, against std::string: find and
// rfind with a position past the end and an empty needle, substr at size(), a count past the end,
// compare of substrings, find_first_of / find_last_not_of from a position, replace and erase with
// counts past the end, insert at size(), resize down and up with a fill, conversions from text.
void show(const char* label, const String& s) { printf("%s [%s] %d\n", label, s.c_str(), (int)s.size()); }
int main()
{
  String s = "abcabcab";
  printf("find %d %d %d %d %d %d\n", (int)s.find("ab", 1), (int)s.find("", 3), (int)s.find("", 8), (int)(s.find("", 9) == String::npos),
         (int)(s.find("ab", 20) == String::npos), (int)s.find('c', 3));
  printf("rfind %d %d %d %d %d\n", (int)s.rfind("ab"), (int)s.rfind("ab", 5), (int)s.rfind("ab", 0), (int)s.rfind(""), (int)s.rfind("", 3));
  printf("rfind %d %d\n", (int)(s.rfind("x") == String::npos), (int)s.rfind('a', 100));
  show("substr", s.substr(8));
  show("substr", s.substr(5, 100));
  show("substr", s.substr(2, 0));
  printf("compare %d %d %d %d\n", s.compare(0, 3, "abc") == 0, s.compare(3, 3, "abc") == 0, s.compare("abd") < 0, s.compare(6, 100, "ab") == 0);
  printf("firstOf %d %d %d %d\n", (int)s.find_first_of("cb", 3), (int)s.find_last_of("a"), (int)s.find_last_not_of("ab"), (int)s.find_first_not_of("abc") == -1);
  printf("firstOf %d %d\n", (int)s.find_last_of("c", 4), (int)s.find_first_not_of("a", 3));
  String r = s;
  r.replace(6, 100, "XYZ");
  show("replace", r);
  r.replace((size_t)0, 0, "<");
  show("replace", r);
  r.erase(4, 100);
  show("erase", r);
  r.insert(r.size(), ">>");
  show("insert", r);
  r.resize(2);
  show("resize", r);
  r.resize(5, '.');
  show("resize", r);
  String t(3, 'z');
  String u("hello world", 5);
  show("ctor", t);
  show("ctor", u);
  printf("ops %d %d %d %d\n", (int)(String("abc") < String("abd")), (int)(String("ab") < String("abc")), (int)(String("b") > String("abc")), (int)(String("") == String()));
  return 0;
}
