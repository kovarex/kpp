// cppinput: clang's and MSVC's `__builtin_is_constant_evaluated()` and `std::is_constant_evaluated()` convert to the
// library's std::isConstantEvaluated() (Unreal's UE_IF_CONSTEVAL), where the builtin was undeclared
#include <type_traits>

constexpr int Pick()
{
  if (__builtin_is_constant_evaluated())
    return 1;
  return 2;
}

constexpr int PickStd()
{
  return std::is_constant_evaluated() ? 10 : 20;
}

int main()
{
  constexpr int AtCompileTime = Pick() + PickStd();
  int volatile Zero = 0;
  int AtRunTime = Pick() + PickStd() + Zero;
  return AtCompileTime == 11 && AtRunTime == 22 ? 0 : 1;
}
