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
using std::move;
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
// battery i19: copies and moves the standard fixes: push_back of an lvalue copies once, of an rvalue
// moves once, emplace_back builds in place, with room reserved; a Vector copied copies each element,
// moved moves none; swap moves none of the elements; Optional's copy and move; a returned local
// moved or elided; an aliasing push_back of the vector's own element; erase's assignments counted
// only by the values left.
struct Counted
{
  int v;
  static int copies, moves, made, gone;
  Counted(int v) : v(v) { ++made; }
  Counted(const Counted& o) : v(o.v) { ++copies; }
  Counted(Counted&& o) noexcept : v(o.v) { o.v = -1; ++moves; }
  Counted& operator=(const Counted& o) { v = o.v; ++copies; return *this; }
  Counted& operator=(Counted&& o) noexcept { v = o.v; o.v = -1; ++moves; return *this; }
  ~Counted() { ++gone; }
};
int Counted::copies = 0, Counted::moves = 0, Counted::made = 0, Counted::gone = 0;
void reset() { Counted::copies = Counted::moves = Counted::made = Counted::gone = 0; }
void report(const char* what) { printf("%s made %d copies %d moves %d gone %d\n", what, Counted::made, Counted::copies, Counted::moves, Counted::gone); reset(); }
Counted makeOne(int v) { Counted c(v); c.v += 1; return c; }
int main()
{
  {
    Vector<Counted> v;
    v.reserve(10);
    Counted a(1);
    v.push_back(a);
    v.push_back(Counted(2));
    v.emplace_back(3);
    report("push");
    v.push_back(v[0]);
    printf("alias %d %d\n", v[3].v, v[0].v);
    report("alias");
    Vector<Counted> w = v;
    report("copy");
    Vector<Counted> x = move(w);
    printf("moved %d %d\n", (int)w.size(), (int)x.size());
    report("move");
    x.swap(v);
    report("swap");
    v.erase(v.begin() + 1);
    printf("erase %d %d %d\n", v[0].v, v[1].v, (int)v.size());
    report("erase");
  }
  report("scope");
  {
    Optional<Counted> o(Counted(5));
    report("optional make");
    Optional<Counted> p = o;
    Optional<Counted> q = move(o);
    printf("optional %d %d %d\n", (int)o.has_value(), p->v, q->v);
    report("optional copy move");
    Counted r = makeOne(8);
    printf("returned %d\n", r.v);
    report("return");
  }
  report("end");
  return 0;
}
