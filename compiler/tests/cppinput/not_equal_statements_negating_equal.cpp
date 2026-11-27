// cppinput: an operator!= whose body is its =='s statement by statement, each return negated (Unreal's TRange:
// `if (IsEmpty() && Other.IsEmpty()) return false; return (Low != Other.Low) || (High != Other.High);` beside an ==
// returning true there and `(Low == Other.Low) && (High == Other.High)`) is dropped, a != b being !(a == b); the
// converter kept it for review, an error in kpp
struct Range
{
  int Low = 0;
  int High = 0;

  bool IsEmpty() const { return High <= Low; }

  bool operator==(const Range& Other) const
  {
    if (IsEmpty() && Other.IsEmpty())
    {
      return true;
    }
    return (Low == Other.Low) && (High == Other.High);
  }

  bool operator!=(const Range& Other) const
  {
    if (IsEmpty() && Other.IsEmpty())
    {
      return false;
    }
    return (Low != Other.Low) || (High != Other.High);
  }
};

int main()
{
  Range a{1, 5}, b{1, 5}, c{2, 5}, e{3, 1}, f{4, 2};
  return a != b || !(a != c) || e != f ? 1 : 0;
}
