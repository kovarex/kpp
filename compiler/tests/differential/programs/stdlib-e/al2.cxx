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
#include <algorithm>
#include <functional>
#include <utility>
#define KPP_NAME(kname, stdname) \
  template <typename... A> auto kname(A&&... a) -> decltype(stdname(std::forward<A>(a)...)) { return stdname(std::forward<A>(a)...); }
KPP_NAME(stableSort, std::stable_sort)
KPP_NAME(nthElement, std::nth_element)
KPP_NAME(removeIf, std::remove_if)
KPP_NAME(nextPermutation, std::next_permutation)
KPP_NAME(lowerBound, std::lower_bound)
KPP_NAME(upperBound, std::upper_bound)
KPP_NAME(equalRange, std::equal_range)
KPP_NAME(binarySearch, std::binary_search)
KPP_NAME(minElement, std::min_element)
KPP_NAME(maxElement, std::max_element)
KPP_NAME(setIntersection, std::set_intersection)
KPP_NAME(setDifference, std::set_difference)
KPP_NAME(lexicographicalCompare, std::lexicographical_compare)
KPP_NAME(isSortedUntil, std::is_sorted_until)
KPP_NAME(adjacentFind, std::adjacent_find)
KPP_NAME(countIf, std::count_if)
KPP_NAME(copyIf, std::copy_if)
KPP_NAME(allOf, std::all_of)
KPP_NAME(anyOf, std::any_of)
KPP_NAME(noneOf, std::none_of)
KPP_NAME(copyBackward, std::copy_backward)
KPP_NAME(fillN, std::fill_n)
KPP_NAME(replaceIf, std::replace_if)
KPP_NAME(findIfNot, std::find_if_not)
template <typename T> using Less = std::less<T>;
using std::sort; using std::unique; using std::remove; using std::rotate; using std::merge; using std::reverse; using std::equal;
#include <deque>
#include <list>
#include <set>
#include <memory>
#include <numeric>
#include <iterator>
#include <cmath>
template <typename T> using Deque = std::deque<T>;
template <typename T> using List = std::list<T>;
template <typename T> using Set = std::set<T>;
template <typename T> using UniquePtr = std::unique_ptr<T>;
using std::make_unique; using std::distance; using std::next; using std::accumulate; using std::transform; using std::is_sorted;
#define makeUnique make_unique
#define backInserter std::back_inserter
#define isSorted std::is_sorted
// <algorithm> over the library's containers and element types: Deque's, List's and Set's iterators,
// reverse iterators, move-only and String elements, output through backInserter; and large sorts
// (sorted, a permutation of the input, stable where promised) on patterns that break naive
// quicksorts: sorted, reversed, all equal, organ pipe, few distinct values, random.
unsigned seed = 12345;
int nextRandom()
{
  seed = seed * 1103515245u + 12345u;
  return (int)((seed >> 8) & 0xffffff);
}
struct Keyed
{
  int key;
  int order;
};
template <typename C> void printAll(const char* label, const C& c)
{
  printf("%s:", label);
  for (const auto& x : c)
    printf(" %d", (int)x);
  printf("\n");
}
void printStrings(const char* label, const Vector<String>& v)
{
  printf("%s:", label);
  for (const String& s : v)
    printf(" [%s]", s.c_str());
  printf("\n");
}
long long comparisons = 0;
void checkSort(const char* pattern, Vector<int>& v)
{
  long long sum = 0, squares = 0;
  for (int x : v)
  {
    sum += x;
    squares += (long long)x * x;
  }
  comparisons = 0;
  sort(v.begin(), v.end(), [](int a, int b) {
    ++comparisons;
    return a < b;
  });
  long long sum2 = 0, squares2 = 0;
  bool sorted = true;
  for (size_t i = 0; i < v.size(); ++i)
  {
    sum2 += v[i];
    squares2 += (long long)v[i] * v[i];
    if (i > 0 && v[i] < v[i - 1])
      sorted = false;
  }
  double n = (double)v.size();
  printf("sort %s %d: sorted %d same elements %d bounded %d\n", pattern, (int)v.size(), (int)sorted, (int)(sum == sum2 && squares == squares2),
         (int)((double)comparisons <= 4.0 * n * (log2(n) + 1)));
}
void checkStable(const char* pattern, Vector<Keyed>& v)
{
  stableSort(v.begin(), v.end(), [](const Keyed& a, const Keyed& b) { return a.key < b.key; });
  bool ok = true;
  for (size_t i = 1; i < v.size(); ++i)
    if (v[i].key < v[i - 1].key || (v[i].key == v[i - 1].key && v[i].order < v[i - 1].order))
      ok = false;
  printf("stableSort %s %d: sorted and stable %d\n", pattern, (int)v.size(), (int)ok);
}

int main()
{
  // Large sorts.
  int sizes[] = {0, 1, 2, 3, 15, 16, 17, 31, 32, 33, 100, 1000, 100000};
  for (int size : sizes)
  {
    for (int pattern = 0; pattern < 6; ++pattern)
    {
      Vector<int> v;
      Vector<Keyed> k;
      for (int i = 0; i < size; ++i)
      {
        int x = pattern == 0 ? i : pattern == 1 ? size - i : pattern == 2 ? 7 : pattern == 3 ? (i < size / 2 ? i : size - i) : pattern == 4 ? nextRandom() % 3 : nextRandom();
        v.push_back(x);
        k.push_back(Keyed{x % 50, i});
      }
      const char* names[] = {"sorted", "reversed", "equal", "organ", "few", "random"};
      checkSort(names[pattern], v);
      checkStable(names[pattern], k);
    }
  }
  // nthElement on a large random range: the nth value against a sorted copy.
  Vector<int> big;
  for (int i = 0; i < 5000; ++i)
    big.push_back(nextRandom() % 1000);
  Vector<int> sortedBig = big;
  sort(sortedBig.begin(), sortedBig.end());
  int nthOk = 0;
  for (int k = 0; k < 5000; k += 97)
  {
    Vector<int> copy = big;
    nthElement(copy.begin(), copy.begin() + k, copy.end());
    nthOk += copy[k] == sortedBig[k];
  }
  printf("nthElement right %d of 52\n", nthOk);

  // Move-only elements.
  Vector<UniquePtr<int>> owned;
  for (int i = 0; i < 40; ++i)
    owned.push_back(makeUnique<int>((i * 37) % 41));
  sort(owned.begin(), owned.end(), [](const UniquePtr<int>& a, const UniquePtr<int>& b) { return *a < *b; });
  printf("sort UniquePtr:");
  for (const auto& p : owned)
    printf(" %d", *p);
  printf("\n");
  stableSort(owned.begin(), owned.end(), [](const UniquePtr<int>& a, const UniquePtr<int>& b) { return *a % 5 < *b % 5; });
  printf("stableSort UniquePtr by %%5:");
  for (const auto& p : owned)
    printf(" %d", *p);
  printf("\n");
  rotate(owned.begin(), owned.begin() + 7, owned.end());
  auto kept = removeIf(owned.begin(), owned.end(), [](const UniquePtr<int>& p) { return *p % 3 == 0; });
  owned.erase(kept, owned.end());
  printf("rotate, removeIf UniquePtr:");
  for (const auto& p : owned)
    printf(" %d", *p);
  printf("\n");

  // Strings, short and long (past any small-string buffer).
  Vector<String> words;
  const char* texts[] = {"pear", "apple", "a fairly long string that is not small at all", "fig", "apple", "", "zebra", "a fairly long string that is not small at all, longer",
                         "kiwi", "fig"};
  for (const char* t : texts)
    words.push_back(String(t));
  Vector<String> w = words;
  sort(w.begin(), w.end());
  printStrings("sort strings", w);
  w.erase(unique(w.begin(), w.end()), w.end());
  printStrings("unique strings", w);
  w = words;
  stableSort(w.begin(), w.end(), [](const String& a, const String& b) { return a.size() < b.size(); });
  printStrings("stableSort strings by size", w);
  w = words;
  rotate(w.begin(), w.begin() + 3, w.end());
  reverse(w.begin(), w.end());
  printStrings("rotate, reverse strings", w);
  w = words;
  w.erase(remove(w.begin(), w.end(), String("fig")), w.end());
  printStrings("remove fig", w);
  w = words;
  sort(w.rbegin(), w.rend());
  printStrings("sort through reverse iterators", w);
  printf("lowerBound kiwi in descending, reversed: %d\n", (int)(lowerBound(w.rbegin(), w.rend(), String("kiwi")) - w.rbegin()));

  // Deque (random access, blocks): sort, nthElement, binary searches across block edges.
  Deque<int> dq;
  for (int i = 0; i < 3000; ++i)
    dq.push_back(nextRandom() % 500);
  for (int i = 0; i < 1000; ++i)
    dq.push_front(nextRandom() % 500);
  sort(dq.begin(), dq.end());
  bool dqSorted = isSorted(dq.begin(), dq.end());
  int dqRange = (int)(upperBound(dq.begin(), dq.end(), 250) - lowerBound(dq.begin(), dq.end(), 250));
  auto dqEqual = equalRange(dq.begin(), dq.end(), 250);
  printf("Deque sorted %d count of 250 %d %d first %d\n", (int)dqSorted, dqRange, (int)(dqEqual.second - dqEqual.first), (int)(dqEqual.first - dq.begin()));
  dq.erase(unique(dq.begin(), dq.end()), dq.end());
  printf("Deque unique size %d\n", (int)dq.size());
  stableSort(dq.begin(), dq.end(), [](int a, int b) { return a % 10 < b % 10; });
  printf("Deque stableSort by %%10 first ten:");
  for (int i = 0; i < 10; ++i)
    printf(" %d", dq[i]);
  printf("\n");

  // List (bidirectional): reverse, nextPermutation, rotate, unique, remove, lowerBound, minElement.
  List<int> list;
  for (int x : {3, 1, 2})
    list.push_back(x);
  for (int i = 0; i < 7; ++i)
  {
    bool more = nextPermutation(list.begin(), list.end());
    printf("List next %d:", (int)more);
    printAll("", list);
  }
  List<int> l2;
  for (int x : {1, 1, 2, 3, 3, 3, 4, 5, 5, 6})
    l2.push_back(x);
  auto l2end = unique(l2.begin(), l2.end());
  printf("List unique kept %d\n", (int)distance(l2.begin(), l2end));
  l2.erase(l2end, l2.end());
  printAll("List unique", l2);
  auto middle = next(l2.begin(), 2);
  auto rotated = rotate(l2.begin(), middle, l2.end());
  printf("List rotate result %d\n", (int)distance(l2.begin(), rotated));
  printAll("List rotate", l2);
  reverse(l2.begin(), l2.end());
  printAll("List reverse", l2);
  printf("List minElement %d maxElement %d\n", (int)distance(l2.begin(), minElement(l2.begin(), l2.end())), (int)distance(l2.begin(), maxElement(l2.begin(), l2.end())));
  l2.erase(remove(l2.begin(), l2.end(), 3), l2.end());
  printAll("List remove 3", l2);
  List<int> sortedList;
  for (int x : {1, 2, 2, 4, 8, 8, 8, 9})
    sortedList.push_back(x);
  for (int v : {0, 2, 3, 8, 10})
    printf("List %d: lower %d upper %d found %d\n", v, (int)distance(sortedList.begin(), lowerBound(sortedList.begin(), sortedList.end(), v)),
           (int)distance(sortedList.begin(), upperBound(sortedList.begin(), sortedList.end(), v)), (int)binarySearch(sortedList.begin(), sortedList.end(), v));
  printAll("List reversed through rbegin", Vector<int>(sortedList.rbegin(), sortedList.rend()));

  // Set (bidirectional, const): searches, counting, copying out.
  Set<int> set;
  for (int x : {9, 3, 7, 1, 5})
    set.insert(x);
  printf("Set lowerBound 4 %d count odd %d\n", (int)distance(set.begin(), lowerBound(set.begin(), set.end(), 4)), (int)countIf(set.begin(), set.end(), [](int x) { return x % 2; }));
  Vector<int> fromSet;
  copyIf(set.begin(), set.end(), backInserter(fromSet), [](int x) { return x > 2; });
  printAll("Set copyIf >2", fromSet);

  // Output through backInserter; merge and set operations with a descending order.
  Vector<int> d1, d2, merged, inter, diff;
  for (int x : {9, 7, 7, 4, 1})
    d1.push_back(x);
  for (int x : {8, 7, 4, 4, 0})
    d2.push_back(x);
  merge(d1.begin(), d1.end(), d2.begin(), d2.end(), backInserter(merged), [](int a, int b) { return a > b; });
  printAll("merge descending", merged);
  setIntersection(d1.begin(), d1.end(), d2.begin(), d2.end(), backInserter(inter), [](int a, int b) { return a > b; });
  printAll("setIntersection descending", inter);
  setDifference(d1.begin(), d1.end(), d2.begin(), d2.end(), backInserter(diff), [](int a, int b) { return a > b; });
  printAll("setDifference descending", diff);
  Vector<int> sums;
  transform(d1.begin(), d1.end(), d2.begin(), backInserter(sums), [](int a, int b) { return a * 10 + b; });
  printAll("transform pairs", sums);
  printf("accumulate %d, strings [%s]\n", accumulate(d1.begin(), d1.end(), 0), accumulate(words.begin(), words.begin() + 3, String(">")).c_str());
  printf("equal 4 %d %d\n", (int)equal(d1.begin(), d1.end(), d1.begin(), d1.end()), (int)equal(d1.begin(), d1.end(), d1.begin(), d1.end() - 1));
  return 0;
}
