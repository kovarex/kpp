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
// <algorithm>: what each guarantees and returns (positions as indices), on duplicates and ties:
// stable sort's order of equal keys, unique and remove's ends, rotate's result, permutations, the
// binary searches on runs, min/max element's tie, merge's order of equals, set operations on
// multisets, nth_element's nth value and partition.
struct Item
{
  int key;
  int order;
};
void print(const char* label, const int* first, const int* last)
{
  printf("%s:", label);
  for (const int* p = first; p != last; ++p)
    printf(" %d", *p);
  printf("\n");
}
void printItems(const char* label, const Item* first, const Item* last)
{
  printf("%s:", label);
  for (const Item* p = first; p != last; ++p)
    printf(" %d.%d", p->key, p->order);
  printf("\n");
}

int main()
{
  Item items[] = {{3, 0}, {1, 1}, {2, 2}, {1, 3}, {3, 4}, {2, 5}, {1, 6}, {0, 7}, {3, 8}, {2, 9}};
  stableSort(items, items + 10, [](const Item& a, const Item& b) { return a.key < b.key; });
  printItems("stableSort", items, items + 10);
  stableSort(items, items + 10, [](const Item& a, const Item& b) { return a.key > b.key; });
  printItems("stableSort desc", items, items + 10);

  int a[] = {5, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5, 8, 9, 7, 9};
  int n = 15;
  int s[15];
  for (int i = 0; i < n; ++i)
    s[i] = a[i];
  sort(s, s + n);
  print("sort", s, s + n);
  int u[15];
  for (int i = 0; i < n; ++i)
    u[i] = s[i];
  int* uend = unique(u, u + n);
  printf("unique end %d\n", (int)(uend - u));
  print("unique", u, uend);
  int r[15];
  for (int i = 0; i < n; ++i)
    r[i] = a[i];
  int* rend = remove(r, r + n, 5);
  printf("remove 5 end %d\n", (int)(rend - r));
  print("remove 5", r, rend);
  int ri[15];
  for (int i = 0; i < n; ++i)
    ri[i] = a[i];
  int* riend = removeIf(ri, ri + n, [](int x) { return x % 2 == 0; });
  print("removeIf even", ri, riend);
  int rot[] = {1, 2, 3, 4, 5, 6, 7};
  int* rotated = rotate(rot, rot + 3, rot + 7);
  printf("rotate result %d\n", (int)(rotated - rot));
  print("rotate", rot, rot + 7);
  printf("rotate by 0 result %d\n", (int)(rotate(rot, rot, rot + 7) - rot));
  printf("rotate by all result %d\n", (int)(rotate(rot, rot + 7, rot + 7) - rot));
  int perm[] = {1, 2, 2, 3};
  for (int i = 0; i < 14; ++i)
  {
    bool more = nextPermutation(perm, perm + 4);
    printf("next %d:", (int)more);
    print("", perm, perm + 4);
  }
  int runs[] = {1, 2, 2, 2, 4, 4, 7, 9};
  for (int v = 0; v <= 10; ++v)
  {
    auto range = equalRange(runs, runs + 8, v);
    printf("%d: lower %d upper %d range %d..%d found %d\n", v, (int)(lowerBound(runs, runs + 8, v) - runs), (int)(upperBound(runs, runs + 8, v) - runs),
           (int)(range.first - runs), (int)(range.second - runs), (int)binarySearch(runs, runs + 8, v));
  }
  int ties[] = {4, 1, 7, 1, 7, 3};
  printf("minElement %d maxElement %d\n", (int)(minElement(ties, ties + 6) - ties), (int)(maxElement(ties, ties + 6) - ties));
  Item left[] = {{1, 0}, {2, 1}, {2, 2}, {5, 3}};
  Item right[] = {{2, 10}, {3, 11}, {5, 12}, {5, 13}};
  Item merged[8];
  merge(left, left + 4, right, right + 4, merged, [](const Item& x, const Item& y) { return x.key < y.key; });
  printItems("merge", merged, merged + 8);
  int m1[] = {1, 2, 2, 2, 3, 5, 5};
  int m2[] = {2, 2, 4, 5, 5, 5};
  int out[16];
  int* oe = setIntersection(m1, m1 + 7, m2, m2 + 6, out);
  print("setIntersection", out, oe);
  oe = setDifference(m1, m1 + 7, m2, m2 + 6, out);
  print("setDifference", out, oe);
  int nth[15];
  for (int k = 0; k < n; ++k)
  {
    for (int i = 0; i < n; ++i)
      nth[i] = a[i];
    nthElement(nth, nth + k, nth + n);
    bool before = true, after = true;
    for (int i = 0; i < k; ++i)
      before = before && nth[i] <= nth[k];
    for (int i = k + 1; i < n; ++i)
      after = after && nth[i] >= nth[k];
    printf("nthElement %d: %d partitioned %d\n", k, nth[k], (int)(before && after));
  }
  int l1[] = {1, 2, 3}, l2[] = {1, 2, 3, 0}, l3[] = {1, 3};
  printf("lexicographicalCompare %d %d %d %d\n", (int)lexicographicalCompare(l1, l1 + 3, l2, l2 + 4), (int)lexicographicalCompare(l2, l2 + 4, l1, l1 + 3),
         (int)lexicographicalCompare(l1, l1 + 3, l3, l3 + 2), (int)lexicographicalCompare(l1, l1, l1, l1));
  int partly[] = {1, 2, 2, 5, 3, 4};
  printf("isSortedUntil %d adjacentFind %d\n", (int)(isSortedUntil(partly, partly + 6) - partly), (int)(adjacentFind(partly, partly + 6) - partly));
  printf("countIf odd %d allOf >0 %d anyOf >4 %d noneOf >5 %d\n", (int)countIf(a, a + n, [](int x) { return x % 2; }), (int)allOf(a, a + n, [](int x) { return x > 0; }),
         (int)anyOf(a, a + n, [](int x) { return x > 4; }), (int)noneOf(a, a + n, [](int x) { return x > 9; }));
  int copied[15];
  int* ce = copyIf(a, a + n, copied, [](int x) { return x > 4; });
  print("copyIf >4", copied, ce);
  int back[] = {1, 2, 3, 4, 5, 0, 0};
  copyBackward(back, back + 5, back + 7);
  print("copyBackward overlap", back, back + 7);
  int rev[] = {1, 2, 3, 4};
  reverse(rev, rev + 4);
  print("reverse", rev, rev + 4);
  printf("equal %d %d\n", (int)equal(l1, l1 + 3, l2), (int)equal(l1, l1 + 3, l3));
  printf("findIfNot <3 %d\n", (int)(findIfNot(a, a + n, [](int x) { return x < 3; }) - a));
  int filled[6] = {0};
  int* fe = fillN(filled, 4, 9);
  printf("fillN end %d\n", (int)(fe - filled));
  print("fillN", filled, filled + 6);
  return 0;
}
