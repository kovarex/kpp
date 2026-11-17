#include <stdio.h>
#include <compare>
#include <compare>
#include <string.h>
#include <stdint.h>
#include <initializer_list>
#include <new>
#include <type_traits>
template <class T, class... A> constexpr bool isConstructible = std::is_constructible_v<T, A...>;
template <class T, class... A> constexpr bool isNothrowConstructible = std::is_nothrow_constructible_v<T, A...>;
template <class T> constexpr bool isTriviallyCopyable = std::is_trivially_copyable_v<T>;
template <class T> constexpr bool isTriviallyDestructible = std::is_trivially_destructible_v<T>;
template <class T> constexpr bool isNothrowDestructible = std::is_nothrow_destructible_v<T>;
template <class T> constexpr bool isTriviallyDefaultConstructible = std::is_trivially_default_constructible_v<T>;
template <class T> constexpr bool isTriviallyCopyConstructible = std::is_trivially_copy_constructible_v<T>;
template <class T> constexpr bool isTriviallyMoveConstructible = std::is_trivially_move_constructible_v<T>;
template <class T> constexpr bool isTriviallyCopyAssignable = std::is_trivially_copy_assignable_v<T>;
template <class T> constexpr bool isTriviallyMoveAssignable = std::is_trivially_move_assignable_v<T>;
template <class T> constexpr bool isCopyConstructible = std::is_copy_constructible_v<T>;
template <class T> constexpr bool isMoveConstructible = std::is_move_constructible_v<T>;
template <class T> constexpr bool isCopyAssignable = std::is_copy_assignable_v<T>;
template <class T> constexpr bool isMoveAssignable = std::is_move_assignable_v<T>;
template <class T> constexpr bool isNothrowMoveConstructible = std::is_nothrow_move_constructible_v<T>;
template <class T> constexpr bool isNothrowMoveAssignable = std::is_nothrow_move_assignable_v<T>;
template <class T> constexpr bool isDefaultConstructible = std::is_default_constructible_v<T>;
template <class T> constexpr bool isClass = std::is_class_v<T>;
template <class T> constexpr bool isEmpty = std::is_empty_v<T>;
struct Agg { int a; double b; };
struct NoDefault { NoDefault() = delete; IMPLICIT NoDefault(int) {} };
int main()
{
  printf("%d%d%d%d %d%d%d%d %d%d%d\n", isConstructible<int[3], int>, isConstructible<int[3], int, int, int>, isConstructible<int[3], int, int, int, int>, isConstructible<int[3]>,
         isConstructible<int[], int>, isConstructible<int[2], const char*>, isConstructible<Agg[2], Agg>, isConstructible<int[3], double>,
         isConstructible<NoDefault[2], int>, isConstructible<NoDefault[2], int, int>, isConstructible<int[2][2], int>);
  return 0;
}
