// The C++ side of a stdlib battery: std's types under kpp's names.
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <string>
#include <string_view>
#include <array>
#include <cstring>
#include <vector>
#include <optional>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <list>
#include <forward_list>
#include <queue>
#include <stack>
#include <algorithm>
#include <numeric>
#include <iterator>
#include <memory>
#include <utility>
#include <tuple>
#include <bitset>
#include <span>
#include <chrono>
#include <filesystem>
#include <stdexcept>
using String = std::string;
using StringView = std::string_view;
using WString = std::wstring;
template <class T> using Vector = std::vector<T>;
template <class T> using Optional = std::optional<T>;
template <class T> using Function = std::function<T>;
template <class K, class V> using Map = std::map<K, V>;
template <class K, class V> using MultiMap = std::multimap<K, V>;
template <class K> using Set = std::set<K>;
template <class K> using MultiSet = std::multiset<K>;
template <class K, class V> using HashMap = std::unordered_map<K, V>;
template <class K> using HashSet = std::unordered_set<K>;
template <class T> using Deque = std::deque<T>;
template <class T> using List = std::list<T>;
template <class T> using ForwardList = std::forward_list<T>;
template <class T> using Queue = std::queue<T>;
template <class T> using Stack = std::stack<T>;
template <class T> using PriorityQueue = std::priority_queue<T>;
template <class A, class B> using Pair = std::pair<A, B>;
template <class T> using Hash = std::hash<T>;
template <class T> using UniquePtr = std::unique_ptr<T>;
template <class T> using SharedPtr = std::shared_ptr<T>;
template <class T> using WeakPtr = std::weak_ptr<T>;
template <class T, size_t N> using Array = std::array<T, N>;
template <size_t N> using Bitset = std::bitset<N>;
template <class... T> using Tuple = std::tuple<T...>;
using std::nullopt;
using std::move;
using std::swap;
using std::min;
using std::max;
using std::sort;
using std::stable_sort;
using std::reverse;
using std::rotate;
using std::unique;
using std::accumulate;
using std::fill;
using std::transform;
using std::merge;
using std::stoll;
using std::stoi;
using std::stod;
namespace filesystem = std::filesystem;
#define KPP_FORWARD(kname, stdname) \
  template <class... A> decltype(auto) kname(A&&... a) { return stdname(std::forward<A>(a)...); }
KPP_FORWARD(stableSort, std::stable_sort)
KPP_FORWARD(findIf, std::find_if)
KPP_FORWARD(findIfNot, std::find_if_not)
KPP_FORWARD(removeIf, std::remove_if)
KPP_FORWARD(copyIf, std::copy_if)
KPP_FORWARD(lowerBound, std::lower_bound)
KPP_FORWARD(upperBound, std::upper_bound)
KPP_FORWARD(equalRange, std::equal_range)
KPP_FORWARD(binarySearch, std::binary_search)
KPP_FORWARD(nthElement, std::nth_element)
KPP_FORWARD(nextPermutation, std::next_permutation)
KPP_FORWARD(maxElement, std::max_element)
KPP_FORWARD(minElement, std::min_element)
KPP_FORWARD(allOf, std::all_of)
KPP_FORWARD(anyOf, std::any_of)
KPP_FORWARD(noneOf, std::none_of)
KPP_FORWARD(isSorted, std::is_sorted)
KPP_FORWARD(isSortedUntil, std::is_sorted_until)
KPP_FORWARD(adjacentFind, std::adjacent_find)
KPP_FORWARD(setIntersection, std::set_intersection)
KPP_FORWARD(setDifference, std::set_difference)
KPP_FORWARD(lexicographicalCompare, std::lexicographical_compare)
KPP_FORWARD(backInserter, std::back_inserter)
KPP_FORWARD(forEach, std::for_each)
KPP_FORWARD(copyN, std::copy_n)
KPP_FORWARD(fillN, std::fill_n)
KPP_FORWARD(iterSwap, std::iter_swap)
template <class T, class... A> std::shared_ptr<T> makeShared(A&&... a) { return std::make_shared<T>(std::forward<A>(a)...); }
template <class T, class... A> std::unique_ptr<T> makeUnique(A&&... a) { return std::make_unique<T>(std::forward<A>(a)...); }
KPP_FORWARD(makePair, std::make_pair)
KPP_FORWARD(makeTuple, std::make_tuple)
#include <format>
using std::format;
// kpp's formatting pieces, as the C runtime's printf writes them (text.md says they are its).
inline std::string printfText(const char* pattern, ...)
{
  char buffer[512];
  va_list args;
  va_start(args, pattern);
  vsnprintf(buffer, sizeof(buffer), pattern, args);
  va_end(args);
  return buffer;
}
inline std::string fixed(double x, int decimals) { return printfText("%.*f", decimals, x); }
inline std::string significant(double x, int digits) { return printfText("%.*g", digits, x); }
inline std::string scientific(double x, int digits) { return printfText("%.*e", digits, x); }
inline std::string hex(int x) { return printfText("%x", x); }
inline std::string hex(int x, int digits) { return printfText("%0*x", digits, x); }
inline std::string hex(long long x) { return printfText("%llx", x); }
inline std::string hexUpper(int x) { return printfText("%X", x); }
inline std::string octal(int x) { return printfText("%o", x); }
inline std::string octal(long long x) { return printfText("%llo", x); }
template <class T> std::string pieceText(const T& x) { return std::format("{}", x); }
inline std::string pieceText(const std::string& x) { return x; }
template <class T> std::string alignRight(const T& x, int width) { return printfText("%*s", width, pieceText(x).c_str()); }
template <class T> std::string alignLeft(const T& x, int width) { return printfText("%-*s", width, pieceText(x).c_str()); }
template <class T> std::string withSign(const T& x) { std::string s = pieceText(x); return s[0] == '-' ? s : "+" + s; }
template <class T> std::string zeroPadded(const T& x, int width)
{
  std::string s = pieceText(x);
  size_t sign = (s[0] == '-' || s[0] == '+') ? 1 : 0;
  if ((int)s.size() < width)
    s.insert(sign, width - s.size(), '0');
  return s;
}
// chrono under kpp's names
template <class Rep, class Period = std::ratio<1>> using Duration = std::chrono::duration<Rep, Period>;
template <long long N, long long D = 1> using Ratio = std::ratio<N, D>;
using Nano = std::nano;
using Micro = std::micro;
using Milli = std::milli;
using Nanoseconds = std::chrono::nanoseconds;
using Microseconds = std::chrono::microseconds;
using Milliseconds = std::chrono::milliseconds;
using Seconds = std::chrono::seconds;
using Minutes = std::chrono::minutes;
using Hours = std::chrono::hours;
using Days = std::chrono::days;
using SystemClock = std::chrono::system_clock;
using SteadyClock = std::chrono::steady_clock;
template <class C, class D = typename C::duration> using TimePoint = std::chrono::time_point<C, D>;
template <class To, class R, class P> To durationCast(const Duration<R, P>& d) { return std::chrono::duration_cast<To>(d); }
template <class To, class C, class D> TimePoint<C, To> timePointCast(const TimePoint<C, D>& t) { return std::chrono::time_point_cast<To>(t); }
using std::chrono::floor;
using std::chrono::ceil;
using InvalidArgument = std::invalid_argument;
using OutOfRange = std::out_of_range;
using LengthError = std::length_error;
using std::stoul;
using std::stoull;
using std::stol;
using std::stof;
template <class T> std::string toString(T x) { return std::to_string(x); }
using std::partition;
using std::search;
using std::mismatch;
using std::count;
using std::minmax;
using std::clamp;
using std::includes;
KPP_FORWARD(stablePartition, std::stable_partition)
KPP_FORWARD(partitionPoint, std::partition_point)
KPP_FORWARD(isPartitioned, std::is_partitioned)
KPP_FORWARD(partitionCopy, std::partition_copy)
KPP_FORWARD(partialSort, std::partial_sort)
KPP_FORWARD(partialSortCopy, std::partial_sort_copy)
KPP_FORWARD(makeHeap, std::make_heap)
KPP_FORWARD(pushHeap, std::push_heap)
KPP_FORWARD(popHeap, std::pop_heap)
KPP_FORWARD(sortHeap, std::sort_heap)
KPP_FORWARD(isHeap, std::is_heap)
KPP_FORWARD(isHeapUntil, std::is_heap_until)
KPP_FORWARD(setUnion, std::set_union)
KPP_FORWARD(setSymmetricDifference, std::set_symmetric_difference)
KPP_FORWARD(inplaceMerge, std::inplace_merge)
KPP_FORWARD(findEnd, std::find_end)
KPP_FORWARD(searchN, std::search_n)
KPP_FORWARD(findFirstOf, std::find_first_of)
KPP_FORWARD(isPermutation, std::is_permutation)
KPP_FORWARD(countIf, std::count_if)
KPP_FORWARD(removeCopy, std::remove_copy)
KPP_FORWARD(removeCopyIf, std::remove_copy_if)
KPP_FORWARD(replaceCopy, std::replace_copy)
KPP_FORWARD(replaceCopyIf, std::replace_copy_if)
KPP_FORWARD(uniqueCopy, std::unique_copy)
KPP_FORWARD(reverseCopy, std::reverse_copy)
KPP_FORWARD(rotateCopy, std::rotate_copy)
KPP_FORWARD(swapRanges, std::swap_ranges)
KPP_FORWARD(forEachN, std::for_each_n)
KPP_FORWARD(prevPermutation, std::prev_permutation)
KPP_FORWARD(minmaxElement, std::minmax_element)
template <class T> using TupleSize = std::tuple_size<T>;
using std::get;
template <class T, size_t N> auto toArray(T (&a)[N]) { return std::to_array(a); }
template <class T, size_t N> auto toArray(T (&&a)[N]) { return std::to_array(std::move(a)); }
