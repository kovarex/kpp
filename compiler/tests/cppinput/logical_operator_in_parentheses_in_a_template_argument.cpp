// cppinput: `&&` or `||` in parentheses inside a template argument does not make the list a
// comparison (Unreal's UE_STATIC_DEPRECATE: `TStaticDeprecateExpression<!(A<K> && A<V>)>()`)
template<bool B> struct Flag
{
  static constexpr int value = B ? 1 : 2;
};
template<typename T> constexpr bool small = sizeof(T) < 4;
int pick(Flag<true>) { return 1; }
int pick(Flag<false>) { return 2; }

int main()
{
  int a = pick(Flag<!(small<char> && small<short>)>());
  int b = pick(Flag<(small<char> || small<long long>)>());
  return a == 2 && b == 1 ? 0 : 1;
}
