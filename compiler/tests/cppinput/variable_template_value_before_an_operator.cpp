// cppinput: a variable template's value with `*` after it (Unreal's TVariantMeta.h: `TVariantSize_V<Variant0> *
// EncodeIndices(Rest...)`), which kpp would read as the comparisons `a < b > *c`: written in parentheses
template <typename T>
constexpr int sizeOf = sizeof(T);

template <typename T>
int scaled(int n)
{
  return 1 + sizeOf<T> * n;
}

int main()
{
  return scaled<int>(2) == 9 ? 0 : 1;
}
