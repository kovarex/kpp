// cppinput: a type named in capitals before a variable template's specialisation is the variable's
// type, not a macro before a template type (Unreal's TVariantMeta.h: `constexpr SIZE_T
// TVariantSize_V<TVariant<Ts...>> = sizeof...(Ts);` declared nothing)
typedef unsigned long long SIZE_T;
template<typename... Ts> struct Variant
{
};
template<typename T> constexpr SIZE_T variantSize = 0;
template<typename... Ts> constexpr SIZE_T variantSize<Variant<Ts...>> = sizeof...(Ts);
template<> constexpr SIZE_T variantSize<int> = 1;

int main()
{
  return variantSize<Variant<int, char>> == 2 && variantSize<int> == 1 ? 0 : 1;
}
