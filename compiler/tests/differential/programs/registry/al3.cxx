#include <compare>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <initializer_list>
#include <new>
// kpp's names for battery al3 over the C++ standard library (after the includes, so the macros touch
// only the battery's text).
#include <vector>
#include <list>
#include <set>
#include <string>
#include <algorithm>
#include <numeric>
#include <iterator>
#include <functional>
#include <utility>
template <typename T> using Vector = std::vector<T>;
template <typename T> using List = std::list<T>;
template <typename T> using Set = std::set<T>;
template <typename A, typename B> using Pair = std::pair<A, B>;
using String = std::string;
template <typename T = void> using Less = std::less<T>;
template <typename T = void> using Greater = std::greater<T>;
using i8 = signed char;
using std::move;
#define sort std::sort
#define copy std::copy
#define partition std::partition
#define stablePartition std::stable_partition
#define isPartitioned std::is_partitioned
#define partitionPoint std::partition_point
#define partitionCopy std::partition_copy
#define makeHeap std::make_heap
#define pushHeap std::push_heap
#define popHeap std::pop_heap
#define sortHeap std::sort_heap
#define isHeap std::is_heap
#define isHeapUntil std::is_heap_until
#define partialSort std::partial_sort
#define partialSortCopy std::partial_sort_copy
#define inplaceMerge std::inplace_merge
#define setUnion std::set_union
#define setIntersection std::set_intersection
#define setDifference std::set_difference
#define setSymmetricDifference std::set_symmetric_difference
#define includes std::includes
#define search std::search
#define findEnd std::find_end
#define searchN std::search_n
#define findFirstOf std::find_first_of
#define mismatch std::mismatch
#define isPermutation std::is_permutation
#define prevPermutation std::prev_permutation
#define nextPermutation std::next_permutation
#define minmaxElement std::minmax_element
#define minElement std::min_element
#define maxElement std::max_element
#define minmax std::minmax
#define clamp std::clamp
#define removeCopy std::remove_copy
#define removeCopyIf std::remove_copy_if
#define replaceCopy std::replace_copy
#define replaceCopyIf std::replace_copy_if
#define replaceIf std::replace_if
#define uniqueCopy std::unique_copy
#define reverseCopy std::reverse_copy
#define rotateCopy std::rotate_copy
#define swapRanges std::swap_ranges
#define shiftLeft std::shift_left
#define shiftRight std::shift_right
#define iota std::iota
#define partialSum std::partial_sum
#define adjacentDifference std::adjacent_difference
#define innerProduct std::inner_product
#define reduce std::reduce
#define transformReduce std::transform_reduce
#define inclusiveScan std::inclusive_scan
#define exclusiveScan std::exclusive_scan
#define transformInclusiveScan std::transform_inclusive_scan
#define transformExclusiveScan std::transform_exclusive_scan
#define generate std::generate
#define forEachN std::for_each_n
#define advance std::advance
#define next std::next
#define prev std::prev
#define distance std::distance
#define inserter std::inserter
#define backInserter std::back_inserter
#define makeMoveIterator std::make_move_iterator
#define makeReverseIterator std::make_reverse_iterator
#define ssize std::ssize
#define iterSwap std::iter_swap
// al3 (thread I, 2026-11-09): the algorithms and numeric functions no battery had compared, against
// std (MSVC's STL, libstdc++): partitions, heaps, partial sorts, inplace_merge, the set operations on
// runs of equal elements (which range each copy comes from), the searches, permutations backwards,
// minmax and its ties, the copying forms, shift_left/right, the numeric scans and their accumulator
// types, and the iterator helpers. Positions print as indices; parts whose order the standard leaves
// open print sorted.
struct Keyed
{
  int key;
  int order;
  bool operator<(const Keyed& o) const { return key < o.key; }
  bool operator==(const Keyed& o) const { return key == o.key; }
};

template <typename C> void show(const char* label, const C& c)
{
  printf("%s:", label);
  for (const auto& x : c)
    printf(" %d", (int)x);
  printf("\n");
}

template <typename It> void showRange(const char* label, It first, It last)
{
  printf("%s:", label);
  for (; first != last; ++first)
    printf(" %d", (int)*first);
  printf("\n");
}

template <typename It> void showKeyed(const char* label, It first, It last)
{
  printf("%s:", label);
  for (; first != last; ++first)
    printf(" %d.%d", first->key, first->order);
  printf("\n");
}

template <typename It> void showSorted(const char* label, It first, It last)
{
  Vector<int> values;
  for (; first != last; ++first)
    values.push_back(*first);
  sort(values.begin(), values.end());
  show(label, values);
}

Vector<Keyed> keyed(const Vector<int>& keys, int base)
{
  Vector<Keyed> result;
  for (int i = 0; i < (int)keys.size(); ++i)
    result.push_back(Keyed{keys[i], base + i});
  return result;
}

bool isOdd(int x) { return x % 2 != 0; }
bool isSmall(int x) { return x < 5; }
bool keyOdd(const Keyed& k) { return k.key % 2 != 0; }
bool lessKeyed(const Keyed& a, const Keyed& b) { return a.key < b.key; }

void partitions()
{
  Vector<int> base = {5, 2, 9, 4, 7, 6, 1, 8, 3, 0, 11, 10};
  {
    Vector<int> v = base;
    auto p = partition(v.begin(), v.end(), isOdd);
    printf("partition at %d, partitioned %d\n", (int)(p - v.begin()), (int)isPartitioned(v.begin(), v.end(), isOdd));
    showSorted("partition true part", v.begin(), p);
    showSorted("partition false part", p, v.end());
  }
  {
    Vector<int> none;
    auto p = partition(none.begin(), none.end(), isOdd);
    printf("partition of none at %d\n", (int)(p - none.begin()));
    Vector<int> odd = {1, 3, 5};
    p = partition(odd.begin(), odd.end(), isOdd);
    printf("partition of all true at %d\n", (int)(p - odd.begin()));
    Vector<int> even = {2, 4, 6};
    p = partition(even.begin(), even.end(), isOdd);
    printf("partition of all false at %d\n", (int)(p - even.begin()));
  }
  {
    List<int> l = {5, 2, 9, 4, 7, 6, 1, 8};
    auto p = partition(l.begin(), l.end(), isOdd);
    printf("list partition at %d, partitioned %d\n", (int)distance(l.begin(), p), (int)isPartitioned(l.begin(), l.end(), isOdd));
    showSorted("list partition true part", l.begin(), p);
  }
  {
    Vector<Keyed> k = keyed(base, 0);
    auto p = stablePartition(k.begin(), k.end(), keyOdd);
    printf("stable_partition at %d\n", (int)(p - k.begin()));
    showKeyed("stable_partition", k.begin(), k.end());
    Vector<Keyed> all = keyed(Vector<int>{1, 3, 5, 7}, 0);
    p = stablePartition(all.begin(), all.end(), keyOdd);
    printf("stable_partition of all true at %d\n", (int)(p - all.begin()));
    Vector<Keyed> none;
    p = stablePartition(none.begin(), none.end(), keyOdd);
    printf("stable_partition of none at %d\n", (int)(p - none.begin()));
  }
  {
    List<Keyed> l;
    for (int i = 0; i < 9; ++i)
      l.push_back(Keyed{(i * 7) % 5, i});
    auto p = stablePartition(l.begin(), l.end(), keyOdd);
    printf("list stable_partition at %d\n", (int)distance(l.begin(), p));
    showKeyed("list stable_partition", l.begin(), l.end());
  }
  {
    Vector<int> v = {1, 3, 5, 7, 2, 4, 6};
    printf("partitioned %d %d %d\n", (int)isPartitioned(v.begin(), v.end(), isOdd), (int)isPartitioned(v.begin() + 4, v.end(), isOdd),
           (int)isPartitioned(base.begin(), base.end(), isOdd));
    printf("partition_point %d %d %d %d\n", (int)(partitionPoint(v.begin(), v.end(), isOdd) - v.begin()),
           (int)(partitionPoint(v.begin(), v.begin() + 4, isOdd) - v.begin()), (int)(partitionPoint(v.begin() + 4, v.end(), isOdd) - v.begin()),
           (int)(partitionPoint(v.begin(), v.begin(), isOdd) - v.begin()));
    List<int> l = {1, 3, 2, 4, 6};
    printf("list partition_point %d\n", (int)distance(l.begin(), partitionPoint(l.begin(), l.end(), isOdd)));
  }
  {
    Vector<int> yes(12, -1), no(12, -1);
    auto ends = partitionCopy(base.begin(), base.end(), yes.begin(), no.begin(), isSmall);
    printf("partition_copy ends %d %d\n", (int)(ends.first - yes.begin()), (int)(ends.second - no.begin()));
    showRange("partition_copy true", yes.begin(), ends.first);
    showRange("partition_copy false", no.begin(), ends.second);
  }
}

void heaps()
{
  Vector<int> base = {3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5, 8, 9, 7, 9};
  {
    Vector<int> v = base;
    makeHeap(v.begin(), v.end());
    printf("make_heap: heap %d, front %d\n", (int)isHeap(v.begin(), v.end()), v.front());
    Vector<int> popped;
    int heapsKept = 0;
    for (auto end = v.end(); end != v.begin(); --end)
    {
      popHeap(v.begin(), end);
      popped.push_back(*(end - 1));
      if (isHeap(v.begin(), end - 1))
        ++heapsKept;
    }
    show("pop_heap order", popped);
    printf("heaps kept %d\n", heapsKept);
    show("popped in place", v);
  }
  {
    Vector<int> h;
    int heapsKept = 0;
    for (int x : base)
    {
      h.push_back(x);
      pushHeap(h.begin(), h.end());
      if (isHeap(h.begin(), h.end()))
        ++heapsKept;
      printf(" %d", h.front());
    }
    printf(" (push_heap fronts), heaps %d\n", heapsKept);
    sortHeap(h.begin(), h.end());
    show("sort_heap", h);
  }
  {
    Vector<int> v = base;
    makeHeap(v.begin(), v.end(), Greater<int>());
    printf("min heap: heap %d, front %d\n", (int)isHeap(v.begin(), v.end(), Greater<int>()), v.front());
    sortHeap(v.begin(), v.end(), Greater<int>());
    show("min heap sort_heap", v);
  }
  {
    Vector<int> v = {9, 5, 8, 6, 7, 10, 1};
    printf("is_heap_until %d\n", (int)(isHeapUntil(v.begin(), v.end()) - v.begin()));
    Vector<int> w = {9, 9, 9, 9, 10};
    printf("is_heap_until of equals %d\n", (int)(isHeapUntil(w.begin(), w.end()) - w.begin()));
    Vector<int> one = {4};
    printf("is_heap_until of one %d, of none %d\n", (int)(isHeapUntil(one.begin(), one.end()) - one.begin()),
           (int)(isHeapUntil(one.begin(), one.begin()) - one.begin()));
    Vector<int> m = {1, 2, 3, 2, 1};
    printf("min is_heap_until %d, is_heap %d\n", (int)(isHeapUntil(m.begin(), m.end(), Greater<int>()) - m.begin()),
           (int)isHeap(m.begin(), m.end(), Greater<int>()));
  }
  {
    Vector<Keyed> k = keyed(Vector<int>{2, 1, 2, 3, 1, 3, 2}, 0);
    makeHeap(k.begin(), k.end());
    sortHeap(k.begin(), k.end());
    printf("keyed sort_heap keys:");
    for (const Keyed& x : k)
      printf(" %d", x.key);
    printf("\n");
  }
}

void partialSorts()
{
  Vector<int> base = {9, 3, 7, 1, 8, 2, 6, 4, 5, 0, 3, 7};
  int ks[4] = {0, 1, 5, 12};
  for (int k : ks)
  {
    Vector<int> v = base;
    partialSort(v.begin(), v.begin() + k, v.end());
    printf("partial_sort %d", k);
    showRange(" first", v.begin(), v.begin() + k);
    showSorted("  rest", v.begin() + k, v.end());
  }
  {
    Vector<int> v = base;
    partialSort(v.begin(), v.begin() + 4, v.end(), Greater<int>());
    showRange("partial_sort greater first 4", v.begin(), v.begin() + 4);
  }
  {
    Vector<int> small(4, -1);
    auto e = partialSortCopy(base.begin(), base.end(), small.begin(), small.end());
    printf("partial_sort_copy into 4 ends at %d", (int)(e - small.begin()));
    show("", small);
    Vector<int> large(16, -1);
    e = partialSortCopy(base.begin(), base.end(), large.begin(), large.end());
    printf("partial_sort_copy into 16 ends at %d", (int)(e - large.begin()));
    show("", large);
    Vector<int> none;
    e = partialSortCopy(base.begin(), base.end(), none.begin(), none.end());
    printf("partial_sort_copy into none ends at %d\n", (int)(e - none.begin()));
    e = partialSortCopy(base.begin(), base.begin(), small.begin(), small.end());
    printf("partial_sort_copy of none ends at %d\n", (int)(e - small.begin()));
    List<int> l = {5, 1, 4, 1, 3};
    Vector<int> three(3, -1);
    e = partialSortCopy(l.begin(), l.end(), three.begin(), three.end(), Greater<int>());
    printf("partial_sort_copy of a list, greater, ends at %d", (int)(e - three.begin()));
    show("", three);
  }
}

void merges()
{
  {
    Vector<Keyed> k = keyed(Vector<int>{1, 2, 2, 4, 6}, 0);
    Vector<Keyed> second = keyed(Vector<int>{2, 2, 3, 4, 7, 7}, 100);
    for (const Keyed& x : second)
      k.push_back(x);
    inplaceMerge(k.begin(), k.begin() + 5, k.end());
    showKeyed("inplace_merge", k.begin(), k.end());
  }
  {
    Vector<Keyed> k = keyed(Vector<int>{3, 3, 5}, 0);
    inplaceMerge(k.begin(), k.begin(), k.end());
    inplaceMerge(k.begin(), k.end(), k.end());
    showKeyed("inplace_merge with an empty half", k.begin(), k.end());
  }
  {
    Vector<Keyed> k = keyed(Vector<int>{5, 6, 7, 8}, 0);
    Vector<Keyed> second = keyed(Vector<int>{1, 5, 5, 9}, 100);
    for (const Keyed& x : second)
      k.push_back(x);
    inplaceMerge(k.begin(), k.begin() + 4, k.end(), lessKeyed);
    showKeyed("inplace_merge by a function", k.begin(), k.end());
  }
  {
    List<Keyed> l;
    int keys[8] = {1, 4, 4, 6, 0, 4, 5, 6};
    for (int i = 0; i < 8; ++i)
      l.push_back(Keyed{keys[i], i});
    inplaceMerge(l.begin(), next(l.begin(), 4), l.end());
    showKeyed("list inplace_merge", l.begin(), l.end());
  }
}

void setOperations()
{
  Vector<Keyed> a = keyed(Vector<int>{1, 2, 2, 2, 3, 5, 7, 7}, 0);
  Vector<Keyed> b = keyed(Vector<int>{2, 2, 4, 5, 5, 7, 7, 7, 8}, 100);
  {
    Vector<Keyed> out;
    setUnion(a.begin(), a.end(), b.begin(), b.end(), backInserter(out));
    showKeyed("set_union", out.begin(), out.end());
    out.clear();
    setUnion(b.begin(), b.end(), a.begin(), a.end(), backInserter(out));
    showKeyed("set_union reversed", out.begin(), out.end());
    out.clear();
    setIntersection(a.begin(), a.end(), b.begin(), b.end(), backInserter(out));
    showKeyed("set_intersection", out.begin(), out.end());
    out.clear();
    setDifference(a.begin(), a.end(), b.begin(), b.end(), backInserter(out));
    showKeyed("set_difference", out.begin(), out.end());
    out.clear();
    setDifference(b.begin(), b.end(), a.begin(), a.end(), backInserter(out));
    showKeyed("set_difference reversed", out.begin(), out.end());
    out.clear();
    setSymmetricDifference(a.begin(), a.end(), b.begin(), b.end(), backInserter(out));
    showKeyed("set_symmetric_difference", out.begin(), out.end());
    out.clear();
    setSymmetricDifference(b.begin(), b.end(), a.begin(), a.end(), backInserter(out));
    showKeyed("set_symmetric_difference reversed", out.begin(), out.end());
  }
  {
    Vector<int> x = {1, 2, 2, 3, 5, 5};
    Vector<int> out(20, -1);
    auto e = setUnion(x.begin(), x.end(), x.begin(), x.begin(), out.begin());
    printf("set_union with none ends at %d\n", (int)(e - out.begin()));
    e = setSymmetricDifference(x.begin(), x.end(), x.begin(), x.end(), out.begin());
    printf("set_symmetric_difference with itself ends at %d\n", (int)(e - out.begin()));
    Vector<int> d = {9, 7, 7, 4, 1};
    Vector<int> f = {8, 7, 4, 4, 0};
    e = setUnion(d.begin(), d.end(), f.begin(), f.end(), out.begin(), Greater<int>());
    showRange("set_union greater", out.begin(), e);
    e = setSymmetricDifference(d.begin(), d.end(), f.begin(), f.end(), out.begin(), Greater<int>());
    showRange("set_symmetric_difference greater", out.begin(), e);
  }
  {
    Vector<int> x = {1, 2, 2, 3, 5, 5, 8};
    Vector<int> two2 = {2, 2}, three2 = {2, 2, 2}, mix = {1, 3, 5, 5}, out = {0, 9}, none;
    printf("includes %d %d %d %d %d %d %d\n", (int)includes(x.begin(), x.end(), two2.begin(), two2.end()),
           (int)includes(x.begin(), x.end(), three2.begin(), three2.end()), (int)includes(x.begin(), x.end(), mix.begin(), mix.end()),
           (int)includes(x.begin(), x.end(), out.begin(), out.end()), (int)includes(x.begin(), x.end(), none.begin(), none.end()),
           (int)includes(none.begin(), none.end(), none.begin(), none.end()), (int)includes(none.begin(), none.end(), two2.begin(), two2.end()));
    Vector<int> d = {9, 7, 7, 4, 1}, sub = {7, 7, 1}, notSub = {7, 7, 7};
    printf("includes greater %d %d\n", (int)includes(d.begin(), d.end(), sub.begin(), sub.end(), Greater<int>()),
           (int)includes(d.begin(), d.end(), notSub.begin(), notSub.end(), Greater<int>()));
    Set<int> s = {1, 3, 5, 7};
    List<int> l = {3, 7};
    printf("includes a set a list %d\n", (int)includes(s.begin(), s.end(), l.begin(), l.end()));
  }
}

bool sameDigit(int a, int b) { return a % 10 == b % 10; }
bool bothAbove(int a, int b) { return a > b; }

void searches()
{
  Vector<int> hay = {1, 2, 3, 1, 2, 3, 1, 2, 4, 1, 1, 1, 2};
  Vector<int> n123 = {1, 2, 3}, n12 = {1, 2}, none, n11 = {1, 1}, n5 = {5}, n24 = {2, 4}, n111 = {1, 1, 1, 1};
  Vector<int> longer = hay;
  longer.push_back(7);
  printf("search %d %d %d %d %d %d %d %d\n", (int)(search(hay.begin(), hay.end(), n123.begin(), n123.end()) - hay.begin()),
         (int)(search(hay.begin(), hay.end(), n12.begin(), n12.end()) - hay.begin()),
         (int)(search(hay.begin(), hay.end(), none.begin(), none.end()) - hay.begin()),
         (int)(search(hay.begin(), hay.end(), n11.begin(), n11.end()) - hay.begin()),
         (int)(search(hay.begin(), hay.end(), n5.begin(), n5.end()) - hay.begin()),
         (int)(search(hay.begin(), hay.end(), n24.begin(), n24.end()) - hay.begin()),
         (int)(search(hay.begin(), hay.end(), n111.begin(), n111.end()) - hay.begin()),
         (int)(search(hay.begin(), hay.end(), longer.begin(), longer.end()) - hay.begin()));
  printf("find_end %d %d %d %d %d %d %d %d\n", (int)(findEnd(hay.begin(), hay.end(), n123.begin(), n123.end()) - hay.begin()),
         (int)(findEnd(hay.begin(), hay.end(), n12.begin(), n12.end()) - hay.begin()),
         (int)(findEnd(hay.begin(), hay.end(), none.begin(), none.end()) - hay.begin()),
         (int)(findEnd(hay.begin(), hay.end(), n11.begin(), n11.end()) - hay.begin()),
         (int)(findEnd(hay.begin(), hay.end(), n5.begin(), n5.end()) - hay.begin()),
         (int)(findEnd(hay.begin(), hay.end(), n24.begin(), n24.end()) - hay.begin()),
         (int)(findEnd(hay.begin(), hay.end(), n111.begin(), n111.end()) - hay.begin()),
         (int)(findEnd(hay.begin(), hay.end(), longer.begin(), longer.end()) - hay.begin()));
  Vector<int> tens = {11, 22, 33};
  printf("search by digit %d, find_end by digit %d\n", (int)(search(hay.begin(), hay.end(), tens.begin(), tens.end(), sameDigit) - hay.begin()),
         (int)(findEnd(hay.begin(), hay.end(), tens.begin(), tens.end(), sameDigit) - hay.begin()));
  List<int> lhay = {1, 2, 1, 2, 1, 3};
  List<int> l12 = {1, 2};
  printf("list search %d, find_end %d\n", (int)distance(lhay.begin(), search(lhay.begin(), lhay.end(), l12.begin(), l12.end())),
         (int)distance(lhay.begin(), findEnd(lhay.begin(), lhay.end(), l12.begin(), l12.end())));
  int counts[6] = {0, 1, 2, 3, 4, -1};
  printf("search_n of 1:");
  for (int c : counts)
    printf(" %d", (int)(searchN(hay.begin(), hay.end(), c, 1) - hay.begin()));
  printf("\nsearch_n of 2:");
  for (int c : counts)
    printf(" %d", (int)(searchN(hay.begin(), hay.end(), c, 2) - hay.begin()));
  printf("\nsearch_n above 1:");
  for (int c : counts)
    printf(" %d", (int)(searchN(hay.begin(), hay.end(), c, 1, bothAbove) - hay.begin()));
  printf("\n");
  printf("list search_n %d %d\n", (int)distance(lhay.begin(), searchN(lhay.begin(), lhay.end(), 1, 3)),
         (int)distance(lhay.begin(), searchN(lhay.begin(), lhay.end(), 2, 1)));
  Vector<int> set43 = {4, 3}, set9 = {9};
  printf("find_first_of %d %d %d %d\n", (int)(findFirstOf(hay.begin(), hay.end(), set43.begin(), set43.end()) - hay.begin()),
         (int)(findFirstOf(hay.begin(), hay.end(), none.begin(), none.end()) - hay.begin()),
         (int)(findFirstOf(hay.begin(), hay.end(), set9.begin(), set9.end()) - hay.begin()),
         (int)(findFirstOf(hay.begin(), hay.end(), tens.begin() + 1, tens.end(), sameDigit) - hay.begin()));
  Vector<int> a = {1, 2, 3, 4}, b = {1, 2, 3}, c = {1, 2, 3, 4}, d = {1, 5, 3}, e = {11, 12, 13, 99};
  auto m = mismatch(a.begin(), a.end(), b.begin(), b.end());
  printf("mismatch %d %d", (int)(m.first - a.begin()), (int)(m.second - b.begin()));
  m = mismatch(a.begin(), a.end(), c.begin(), c.end());
  printf(", %d %d", (int)(m.first - a.begin()), (int)(m.second - c.begin()));
  m = mismatch(a.begin(), a.end(), d.begin(), d.end());
  printf(", %d %d", (int)(m.first - a.begin()), (int)(m.second - d.begin()));
  m = mismatch(b.begin(), b.end(), a.begin());
  printf(", %d %d", (int)(m.first - b.begin()), (int)(m.second - a.begin()));
  m = mismatch(a.begin(), a.end(), e.begin(), sameDigit);
  printf(", %d %d", (int)(m.first - a.begin()), (int)(m.second - e.begin()));
  m = mismatch(a.begin(), a.end(), e.begin(), e.end(), sameDigit);
  printf(", %d %d\n", (int)(m.first - a.begin()), (int)(m.second - e.begin()));
  Vector<int> p = {1, 2, 2, 3}, q = {2, 3, 1, 2}, r = {2, 3, 1, 1}, s = {2, 3, 1, 2, 2}, t = {2, 3, 1, 2, 9};
  Vector<int> u = {1, 12, 23}, w = {13, 21, 2};
  printf("is_permutation %d %d %d %d %d %d %d\n", (int)isPermutation(p.begin(), p.end(), q.begin()), (int)isPermutation(p.begin(), p.end(), r.begin()),
         (int)isPermutation(p.begin(), p.end(), s.begin(), s.end()), (int)isPermutation(p.begin(), p.end(), t.begin()),
         (int)isPermutation(none.begin(), none.end(), none.begin(), none.end()), (int)isPermutation(u.begin(), u.end(), w.begin(), sameDigit),
         (int)isPermutation(u.begin(), u.end(), w.begin(), w.end(), sameDigit));
  List<int> lp = {3, 1, 2, 1};
  printf("list is_permutation %d\n", (int)isPermutation(lp.begin(), lp.end(), p.begin(), p.end()));
}

void permutations()
{
  Vector<int> p = {3, 2, 1};
  int steps = 0;
  bool more = true;
  printf("prev_permutation:");
  while (more && steps < 10)
  {
    printf(" %d%d%d", p[0], p[1], p[2]);
    ++steps;
    more = prevPermutation(p.begin(), p.end());
  }
  printf(", then %d%d%d after %d\n", p[0], p[1], p[2], steps);
  Vector<int> d = {2, 2, 1, 1};
  steps = 0;
  more = true;
  printf("prev_permutation with repeats:");
  while (more && steps < 20)
  {
    printf(" %d%d%d%d", d[0], d[1], d[2], d[3]);
    ++steps;
    more = prevPermutation(d.begin(), d.end());
  }
  printf(", then %d%d%d%d after %d\n", d[0], d[1], d[2], d[3], steps);
  Vector<int> one = {5};
  Vector<int> none;
  printf("prev_permutation of one %d, of none %d\n", (int)prevPermutation(one.begin(), one.end()), (int)prevPermutation(none.begin(), none.end()));
  Vector<int> g = {1, 2, 3};
  steps = 0;
  more = true;
  printf("prev_permutation greater:");
  while (more && steps < 10)
  {
    printf(" %d%d%d", g[0], g[1], g[2]);
    ++steps;
    more = prevPermutation(g.begin(), g.end(), Greater<int>());
  }
  printf(", then %d%d%d after %d\n", g[0], g[1], g[2], steps);
  List<int> l = {1, 3, 2};
  more = prevPermutation(l.begin(), l.end());
  showRange("list prev_permutation", l.begin(), l.end());
  printf("list result %d\n", (int)more);
}

void minmaxes()
{
  Vector<int> v = {3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5, 9, 1, 9};
  auto mm = minmaxElement(v.begin(), v.end());
  printf("minmax_element %d %d, min_element %d, max_element %d\n", (int)(mm.first - v.begin()), (int)(mm.second - v.begin()),
         (int)(minElement(v.begin(), v.end()) - v.begin()), (int)(maxElement(v.begin(), v.end()) - v.begin()));
  Vector<Vector<int>> cases = {{}, {7}, {7, 7}, {7, 7, 7}, {7, 7, 7, 7}, {1, 2}, {2, 1}, {2, 1, 2}, {1, 2, 1}, {2, 2, 1, 1}, {1, 1, 2, 2},
                               {5, 1, 5, 1, 5}, {1, 5, 1, 5, 1, 5}, {9, 8, 9, 8, 9, 8, 0, 0}};
  printf("minmax_element:");
  for (const Vector<int>& c : cases)
  {
    auto r = minmaxElement(c.begin(), c.end());
    printf(" %d/%d", (int)(r.first - c.begin()), (int)(r.second - c.begin()));
  }
  printf("\nminmax_element greater:");
  for (const Vector<int>& c : cases)
  {
    auto r = minmaxElement(c.begin(), c.end(), Greater<int>());
    printf(" %d/%d", (int)(r.first - c.begin()), (int)(r.second - c.begin()));
  }
  printf("\n");
  List<int> l = {4, 2, 8, 2, 8, 3};
  auto lr = minmaxElement(l.begin(), l.end());
  printf("list minmax_element %d %d\n", (int)distance(l.begin(), lr.first), (int)distance(l.begin(), lr.second));
  Vector<Keyed> k = keyed(Vector<int>{2, 1, 3, 1, 3, 2, 3, 1}, 0);
  auto km = minmaxElement(k.begin(), k.end());
  printf("keyed minmax_element %d.%d %d.%d\n", km.first->key, km.first->order, km.second->key, km.second->order);
  Keyed a{1, 0}, b{1, 1}, c{2, 2};
  auto p1 = minmax(a, b);
  auto p2 = minmax(c, a);
  auto p3 = minmax(a, c);
  printf("minmax %d.%d %d.%d, %d.%d %d.%d, %d.%d %d.%d\n", p1.first.key, p1.first.order, p1.second.key, p1.second.order, p2.first.key,
         p2.first.order, p2.second.key, p2.second.order, p3.first.key, p3.first.order, p3.second.key, p3.second.order);
  auto p4 = minmax(b, a, lessKeyed);
  printf("minmax by a function %d.%d %d.%d\n", p4.first.key, p4.first.order, p4.second.key, p4.second.order);
  auto p5 = minmax({Keyed{2, 0}, Keyed{1, 1}, Keyed{2, 2}, Keyed{1, 3}, Keyed{2, 4}, Keyed{1, 5}});
  printf("minmax of a list %d.%d %d.%d\n", p5.first.key, p5.first.order, p5.second.key, p5.second.order);
  auto p6 = minmax({Keyed{4, 0}});
  printf("minmax of one %d.%d %d.%d\n", p6.first.key, p6.first.order, p6.second.key, p6.second.order);
  auto p7 = minmax({Keyed{3, 0}, Keyed{3, 1}, Keyed{3, 2}});
  printf("minmax of equals %d.%d %d.%d\n", p7.first.key, p7.first.order, p7.second.key, p7.second.order);
  Keyed lo{3, 1}, hi{5, 2};
  Keyed vs[4] = {Keyed{3, 0}, Keyed{5, 0}, Keyed{6, 0}, Keyed{2, 0}};
  printf("clamp:");
  for (const Keyed& x : vs)
  {
    const Keyed& r = clamp(x, lo, hi);
    printf(" %d.%d", r.key, r.order);
  }
  printf("\n");
}

void copies()
{
  Vector<int> v = {1, 2, 3, 2, 2, 4, 5, 2};
  Vector<int> out(12, -1);
  auto e = removeCopy(v.begin(), v.end(), out.begin(), 2);
  printf("remove_copy ends %d", (int)(e - out.begin()));
  showRange("", out.begin(), e);
  e = removeCopyIf(v.begin(), v.end(), out.begin(), isOdd);
  printf("remove_copy_if ends %d", (int)(e - out.begin()));
  showRange("", out.begin(), e);
  e = replaceCopy(v.begin(), v.end(), out.begin(), 2, 9);
  printf("replace_copy ends %d", (int)(e - out.begin()));
  showRange("", out.begin(), e);
  e = replaceCopyIf(v.begin(), v.end(), out.begin(), isOdd, 0);
  printf("replace_copy_if ends %d", (int)(e - out.begin()));
  showRange("", out.begin(), e);
  Vector<int> w = v;
  replaceIf(w.begin(), w.end(), isOdd, -1);
  show("replace_if", w);
  Vector<int> runs = {1, 1, 2, 2, 2, 3, 1, 1, 4, 4};
  e = uniqueCopy(runs.begin(), runs.end(), out.begin());
  printf("unique_copy ends %d", (int)(e - out.begin()));
  showRange("", out.begin(), e);
  Vector<Keyed> kruns = keyed(Vector<int>{1, 1, 2, 2, 2, 3, 1, 1, 4}, 0);
  Vector<Keyed> kout;
  uniqueCopy(kruns.begin(), kruns.end(), backInserter(kout));
  showKeyed("unique_copy keyed", kout.begin(), kout.end());
  List<int> lout;
  uniqueCopy(runs.begin(), runs.end(), backInserter(lout), sameDigit);
  show("unique_copy into a list", lout);
  Vector<int> none;
  e = uniqueCopy(none.begin(), none.end(), out.begin());
  printf("unique_copy of none ends %d\n", (int)(e - out.begin()));
  e = reverseCopy(v.begin(), v.end(), out.begin());
  printf("reverse_copy ends %d", (int)(e - out.begin()));
  showRange("", out.begin(), e);
  List<int> lv = {1, 2, 3};
  e = reverseCopy(lv.begin(), lv.end(), out.begin());
  showRange("reverse_copy of a list", out.begin(), e);
  int middles[4] = {0, 3, 7, 8};
  for (int mid : middles)
  {
    e = rotateCopy(v.begin(), v.begin() + mid, v.end(), out.begin());
    printf("rotate_copy %d ends %d", mid, (int)(e - out.begin()));
    showRange("", out.begin(), e);
  }
  Vector<int> x = {1, 2, 3, 4, 5}, y = {10, 20, 30, 40, 50};
  auto sr = swapRanges(x.begin(), x.begin() + 3, y.begin() + 1);
  printf("swap_ranges ends %d", (int)(sr - y.begin()));
  show(" x", x);
  show(" y", y);
}

void shifts()
{
  int amounts[7] = {0, 1, 3, 7, 8, 12, 2};
  for (int n : amounts)
  {
    Vector<int> v = {1, 2, 3, 4, 5, 6, 7, 8};
    auto r = shiftLeft(v.begin(), v.end(), n);
    printf("shift_left %d returns %d", n, (int)(r - v.begin()));
    showRange("", v.begin(), r);
    Vector<int> w = {1, 2, 3, 4, 5, 6, 7, 8};
    r = shiftRight(w.begin(), w.end(), n);
    printf("shift_right %d returns %d", n, (int)(r - w.begin()));
    showRange("", r, w.end());
  }
  List<int> l = {1, 2, 3, 4, 5};
  auto lr = shiftLeft(l.begin(), l.end(), 2);
  printf("list shift_left 2 returns %d", (int)distance(l.begin(), lr));
  showRange("", l.begin(), lr);
  List<int> l2 = {1, 2, 3, 4, 5};
  lr = shiftRight(l2.begin(), l2.end(), 2);
  printf("list shift_right 2 returns %d", (int)distance(l2.begin(), lr));
  showRange("", lr, l2.end());
  Vector<String> s = {"one", "two", "three", "four"};
  auto sl = shiftLeft(s.begin(), s.end(), 1);
  printf("strings shift_left 1:");
  for (auto it = s.begin(); it != sl; ++it)
    printf(" %s", it->c_str());
  printf("\n");
}

int square(int x) { return x * x; }

void numerics()
{
  Vector<int> v = {1, 2, 3, 4, 5};
  Vector<int> out(8, 0);
  auto e = partialSum(v.begin(), v.end(), out.begin());
  printf("partial_sum ends %d", (int)(e - out.begin()));
  showRange("", out.begin(), e);
  e = partialSum(v.begin(), v.end(), out.begin(), [](int a, int b) { return a * b; });
  showRange("partial_sum product", out.begin(), e);
  Vector<int> in = v;
  partialSum(in.begin(), in.end(), in.begin());
  show("partial_sum in place", in);
  Vector<int> none;
  e = partialSum(none.begin(), none.end(), out.begin());
  printf("partial_sum of none ends %d\n", (int)(e - out.begin()));
  Vector<i8> bytes = {100, 100, 100, -100};
  Vector<int> wide(4, 0);
  partialSum(bytes.begin(), bytes.end(), wide.begin());
  show("partial_sum of int8 into int", wide);
  Vector<int> sq = {1, 4, 9, 16, 25};
  e = adjacentDifference(sq.begin(), sq.end(), out.begin());
  printf("adjacent_difference ends %d", (int)(e - out.begin()));
  showRange("", out.begin(), e);
  e = adjacentDifference(sq.begin(), sq.end(), out.begin(), [](int a, int b) { return a + b; });
  showRange("adjacent_difference sum", out.begin(), e);
  in = sq;
  adjacentDifference(in.begin(), in.end(), in.begin());
  show("adjacent_difference in place", in);
  e = adjacentDifference(none.begin(), none.end(), out.begin());
  printf("adjacent_difference of none ends %d\n", (int)(e - out.begin()));
  Vector<i8> swing = {100, -100, 100};
  Vector<int> wide3(3, 0);
  adjacentDifference(swing.begin(), swing.end(), wide3.begin());
  show("adjacent_difference of int8 into int", wide3);
  Vector<int> b = {4, 5, 6};
  printf("inner_product %d %d", innerProduct(v.begin(), v.begin() + 3, b.begin(), 0),
         innerProduct(v.begin(), v.begin() + 3, b.begin(), 1, [](int x, int y) { return x * y; }, [](int x, int y) { return x + y; }));
  Vector<double> halves = {0.5, 1.5, 2.5};
  printf(" %g %g\n", innerProduct(halves.begin(), halves.end(), halves.begin(), 0.0), (double)innerProduct(halves.begin(), halves.end(), halves.begin(), 0));
  printf("reduce %d %d %d\n", reduce(v.begin(), v.end(), 0), reduce(none.begin(), none.end(), 7),
         reduce(v.begin(), v.end(), 1, [](int x, int y) { return x * y; }));
  printf("transform_reduce %d %d\n", transformReduce(v.begin(), v.begin() + 3, b.begin(), 0),
         transformReduce(v.begin(), v.end(), 0, [](int x, int y) { return x + y; }, square));
  e = inclusiveScan(v.begin(), v.end(), out.begin());
  printf("inclusive_scan ends %d", (int)(e - out.begin()));
  showRange("", out.begin(), e);
  e = inclusiveScan(v.begin(), v.end(), out.begin(), [](int x, int y) { return x * y; });
  showRange("inclusive_scan product", out.begin(), e);
  e = inclusiveScan(v.begin(), v.end(), out.begin(), [](int x, int y) { return x + y; }, 10);
  showRange("inclusive_scan from 10", out.begin(), e);
  e = exclusiveScan(v.begin(), v.end(), out.begin(), 0);
  printf("exclusive_scan ends %d", (int)(e - out.begin()));
  showRange("", out.begin(), e);
  e = exclusiveScan(v.begin(), v.end(), out.begin(), 1, [](int x, int y) { return x * y; });
  showRange("exclusive_scan product", out.begin(), e);
  in = v;
  exclusiveScan(in.begin(), in.end(), in.begin(), 100);
  show("exclusive_scan in place", in);
  in = v;
  inclusiveScan(in.begin(), in.end(), in.begin());
  show("inclusive_scan in place", in);
  e = exclusiveScan(none.begin(), none.end(), out.begin(), 3);
  printf("exclusive_scan of none ends %d\n", (int)(e - out.begin()));
  Vector<i8> rising = {100, 100, 100};
  Vector<int> wide4(3, 0);
  inclusiveScan(rising.begin(), rising.end(), wide4.begin());
  show("inclusive_scan of int8 into int", wide4);
  inclusiveScan(rising.begin(), rising.end(), wide4.begin(), [](int x, int y) { return x + y; }, 0);
  show("inclusive_scan of int8 from int 0", wide4);
  Vector<double> dout(3, 0.0);
  exclusiveScan(halves.begin(), halves.end(), dout.begin(), 0);
  printf("exclusive_scan of doubles from int 0: %g %g %g\n", dout[0], dout[1], dout[2]);
  inclusiveScan(halves.begin(), halves.end(), dout.begin(), [](double x, double y) { return x + y; }, 0);
  printf("inclusive_scan of doubles from int 0: %g %g %g\n", dout[0], dout[1], dout[2]);
  e = transformInclusiveScan(v.begin(), v.end(), out.begin(), [](int x, int y) { return x + y; }, square);
  showRange("transform_inclusive_scan", out.begin(), e);
  e = transformInclusiveScan(v.begin(), v.end(), out.begin(), [](int x, int y) { return x + y; }, square, 100);
  showRange("transform_inclusive_scan from 100", out.begin(), e);
  e = transformExclusiveScan(v.begin(), v.end(), out.begin(), 0, [](int x, int y) { return x + y; }, square);
  showRange("transform_exclusive_scan", out.begin(), e);
  Vector<int> seq(6, 0);
  iota(seq.begin(), seq.end(), -2);
  show("iota", seq);
  Vector<char> letters(4, 0);
  iota(letters.begin(), letters.end(), 'w');
  show("iota of chars", letters);
  Vector<double> steps(3, 0.0);
  iota(steps.begin(), steps.end(), 0.5);
  printf("iota of doubles %g %g %g\n", steps[0], steps[1], steps[2]);
  int n = 0;
  generate(seq.begin(), seq.end(), [&n] { return n++ * 3; });
  show("generate", seq);
  auto fe = forEachN(seq.begin(), 3, [](int& x) { x = -x; });
  printf("for_each_n ends %d", (int)(fe - seq.begin()));
  show("", seq);
}

void iteratorHelpers()
{
  List<int> l = {1, 2, 3, 4, 5, 6};
  auto it = l.end();
  advance(it, -2);
  printf("advance -2 from the end %d", *it);
  advance(it, -3);
  printf(", -3 more %d", *it);
  advance(it, 4);
  printf(", 4 on %d\n", *it);
  printf("next %d, prev %d %d, distance %d\n", *next(l.begin(), 4), *prev(l.end(), 6), *prev(l.end()), (int)distance(l.begin(), l.end()));
  Vector<int> v = {10, 20, 30, 40, 50};
  printf("vector distance back %d, ssize %d\n", (int)distance(v.end(), v.begin()), (int)ssize(v));
  auto r = makeReverseIterator(v.end());
  auto r2 = makeReverseIterator(v.begin() + 2);
  printf("reverse iterator %d %d, base %d, of 2: %d base %d, rend - rbegin %d, less %d\n", *r, r[2], (int)((r + 1).base() - v.begin()), *r2,
         (int)(r2.base() - v.begin()), (int)(v.rend() - v.rbegin()), (int)(v.rbegin() < v.rend()));
  Vector<int> w = {1, 2, 3};
  Vector<int> add = {7, 8, 9};
  copy(add.begin(), add.end(), inserter(w, w.begin() + 1));
  show("inserter into a vector", w);
  Set<int> s = {5, 1};
  Vector<int> more = {3, 9, 1};
  copy(more.begin(), more.end(), inserter(s, s.begin()));
  show("inserter into a set", s);
  List<int> lw = {1, 2};
  copy(add.begin(), add.end(), inserter(lw, next(lw.begin())));
  show("inserter into a list", lw);
  Vector<String> src = {"alpha", "beta", "gamma"};
  Vector<String> dst;
  copy(makeMoveIterator(src.begin()), makeMoveIterator(src.end()), backInserter(dst));
  printf("move iterator:");
  for (const String& x : dst)
    printf(" %s", x.c_str());
  printf("\n");
  Vector<int> sw = {1, 2, 3};
  iterSwap(sw.begin(), sw.end() - 1);
  show("iter_swap", sw);
}

int main()
{
  partitions();
  heaps();
  partialSorts();
  merges();
  setOperations();
  searches();
  permutations();
  minmaxes();
  copies();
  shifts();
  numerics();
  iteratorHelpers();
  return 0;
}
