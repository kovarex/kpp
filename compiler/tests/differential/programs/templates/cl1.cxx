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
struct Big
{
  int v;
  Big() : v(1) {}
  Big(const Big& o) : v(o.v) {}
  ~Big() {}
};
struct MoveOnly
{
  int v = 0;
  MoveOnly() = default;
  MoveOnly(const MoveOnly&) = delete;
  MoveOnly(MoveOnly&&) = default;
};
struct Throwing
{
  ~Throwing() noexcept(false) {}
};
template <class T> void row(const char* name)
{
  printf("%-10s %d%d%d %d%d%d%d%d%d %d%d%d%d %d%d %d\n", name, isClass<T>, isConstructible<T>, isTriviallyDefaultConstructible<T>,
         isTriviallyCopyable<T>, isTriviallyDestructible<T>, isTriviallyCopyConstructible<T>, isTriviallyMoveConstructible<T>, isTriviallyCopyAssignable<T>, isTriviallyMoveAssignable<T>,
         isCopyConstructible<T>, isMoveConstructible<T>, isCopyAssignable<T>, isMoveAssignable<T>, isNothrowMoveConstructible<T>, isNothrowMoveAssignable<T>,
         isConstructible<T, const T&>);
}
int main()
{
  int cap = 0;
  Big big;
  Throwing throwing;
  auto none = [] { return 1; };
  auto byCopy = [cap] { return cap; };
  auto byRef = [&cap] { return cap; };
  auto defaultCopy = [=] { return cap; };
  auto defaultRef = [&] { return cap; };
  auto bigCopy = [big] { return big.v; };
  auto bigDefault = [=] { return big.v + cap; };
  auto bigRef = [&big] { return big.v; };
  auto moveOnly = [m = MoveOnly()] { return m.v; };
  auto throwingCopy = [=] { (void)throwing; return 0; };
  auto emptyDefault = [=] { return 2; };
  row<decltype(none)>("none");
  row<decltype(byCopy)>("byCopy");
  row<decltype(byRef)>("byRef");
  row<decltype(defaultCopy)>("defCopy");
  row<decltype(defaultRef)>("defRef");
  row<decltype(bigCopy)>("bigCopy");
  row<decltype(bigDefault)>("bigDef");
  row<decltype(bigRef)>("bigRef");
  row<decltype(moveOnly)>("moveOnly");
  row<decltype(throwingCopy)>("throwing");
  row<decltype(emptyDefault)>("emptyDef");
  printf("%d %d %d %d %d\n", noexcept([cap] { return cap; }), noexcept([big] { return big.v; }), noexcept([&big] { return big.v; }),
         noexcept([t = Throwing()] { (void)t; }), noexcept([=] { return big.v; }));
  return none() + byCopy() + byRef() + defaultCopy() + defaultRef() + bigCopy() + bigDefault() + bigRef() + moveOnly() + throwingCopy() + emptyDefault() - 8;
}
