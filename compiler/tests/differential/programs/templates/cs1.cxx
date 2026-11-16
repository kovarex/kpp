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
struct NoCopy
{
  NoCopy() = default;
  NoCopy(const NoCopy&) = delete;
};
struct MoveOnly
{
  MoveOnly() = default;
  MoveOnly(const MoveOnly&) = delete;
  MoveOnly(MoveOnly&&) = default;
};
class PrivateCopy
{
public:
  PrivateCopy() {}
private:
  PrivateCopy(const PrivateCopy&);
};
class Abstract
{
public:
  virtual void f() = 0;
};
struct Plain
{
  int v;
};
template <class T> void row(const char* name)
{
  printf("%-12s %d%d%d%d\n", name, isConstructible<T, const T&>, isConstructible<T, T&>, isConstructible<T, T&&>, isConstructible<T, T>);
}
int main()
{
  int n = 1;
  auto copyable = [n] { return n; };
  auto moveOnly = [m = MoveOnly()] { (void)m; };
  row<NoCopy>("NoCopy");
  row<MoveOnly>("MoveOnly");
  row<PrivateCopy>("PrivateCopy");
  row<Abstract>("Abstract");
  row<Plain>("Plain");
  row<decltype(copyable)>("copyable");
  row<decltype(moveOnly)>("moveOnlyL");
  return copyable() - 1;
}
