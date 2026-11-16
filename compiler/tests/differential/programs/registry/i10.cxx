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
// battery i10: Optional, Tuple, PriorityQueue, StringView, clamp against the standard library's:
// value semantics on copy and move (an empty Optional after assignment, reset, emplace over a value),
// comparisons of Optionals with values and with empties, tuple comparison and tie assignment,
// a priority queue's order with a greater-than comparator and duplicates, string views' finds,
// substrings, comparisons and prefixes, clamp at both ends.
struct Noisy
{
  int v;
  static int live;
  Noisy(int v) : v(v) { ++live; }
  Noisy(const Noisy& o) : v(o.v) { ++live; }
  ~Noisy() { --live; }
};
int Noisy::live = 0;
int main()
{
  Optional<int> a, b = 5;
  Optional<int> c = b;
  a = b;
  b.reset();
  printf("optional %d %d %d %d %d\n", (int)a.has_value(), (int)b.has_value(), *c, a.value_or(9), b.value_or(9));
  printf("compare %d %d %d %d %d %d\n", (int)(a == c), (int)(a < b), (int)(b < a), (int)(a == 5), (int)(b == 5), (int)(b < 1));
  {
    Optional<Noisy> n(3);
    Optional<Noisy> m = n;
    n.emplace(7);
    m.reset();
    printf("noisy %d %d %d\n", Noisy::live, n->v, (int)m.has_value());
    m = n;
    printf("noisy %d %d\n", Noisy::live, m->v);
  }
  printf("noisy after %d\n", Noisy::live);
  Tuple<int, double, char> t1 = makeTuple(1, 2.5, 'a'), t2 = makeTuple(1, 2.5, 'b');
  printf("tuple %d %d %d %g %c\n", (int)(t1 < t2), (int)(t1 == t2), (int)(t2 > t1), get<1>(t1), get<2>(t2));
  int x = 0;
  double y = 0;
  char z = 0;
  tie(x, y, z) = t2;
  get<0>(t1) = 40;
  printf("tie %d %g %c %d\n", x, y, z, get<0>(t1));
  PriorityQueue<int> pq;
  PriorityQueue<int, Vector<int>, Greater<int>> minq;
  int values[] = {5, 1, 9, 3, 9, 0, 7, 3};
  for (int v : values)
  {
    pq.push(v);
    minq.push(v);
  }
  printf("pq");
  while (!pq.empty())
  {
    printf(" %d", pq.top());
    pq.pop();
  }
  printf(" | min");
  while (!minq.empty())
  {
    printf(" %d", minq.top());
    minq.pop();
  }
  printf("\n");
  StringView sv = "hello, world";
  StringView w = sv.substr(7);
  printf("view %d %d %d %d %d %d\n", (int)sv.find('o'), (int)sv.rfind('o'), (int)sv.find("wor"), (int)w.size(), (int)(w == "world"), (int)(sv.compare(w) < 0));
  printf("view %d %d %d\n", (int)sv.starts_with("hell"), (int)sv.ends_with("ld"), (int)(sv.find('z') == StringView::npos));
  printf("clamp %d %d %d %g\n", clamp(5, 1, 3), clamp(-5, 1, 3), clamp(2, 1, 3), clamp(2.5, 0.0, 1.0));
  return 0;
}
