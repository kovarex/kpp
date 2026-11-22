// cppinput: more operator!= forms that are provably the negation of their class's ==, dropped: a field-by-field `!=`
// whose parameter is named apart from =='s (`Other.R` beside `ColorB.R`, Unreal's FLinearColor, TRotator); a test
// beside its == returning the negation (`IsValid()` beside `!IsValid()`, TPimplPtr; `(bool)*this` beside `!*this`,
// TFunction); `false` beside `true` (FNull). An iterator's != with no == and a statement before its return (Unreal's
// TRangedForIterator, `ensure(...); return a != b;`) becomes the == of the negated return
class Color
{
public:
  Color(float InR, float InG) : R(InR), G(InG) {}
  bool operator==(const Color& ColorB) const { return R == ColorB.R && G == ColorB.G; }
  bool operator!=(const Color& Other) const { return R != Other.R || G != Other.G; }

private:
  float R, G;
};

class Handle
{
public:
  explicit Handle(int* InPtr) : Ptr(InPtr) {}
  bool IsValid() const { return Ptr != nullptr; }
  explicit operator bool() const { return IsValid(); }
  bool operator==(decltype(nullptr)) const { return !IsValid(); }
  bool operator!=(decltype(nullptr)) const { return IsValid(); }

private:
  int* Ptr;
};

class Callable
{
public:
  explicit Callable(bool InBound) : Bound(InBound) {}
  explicit operator bool() const { return Bound; }
  bool operator==(decltype(nullptr)) const { return !*this; }
  bool operator!=(decltype(nullptr)) const { return (bool)*this; }

private:
  bool Bound;
};

struct Null
{
  bool operator==(const Null&) const { return true; }
  bool operator!=(const Null&) const { return false; }
};

class Cursor
{
public:
  Cursor(int InIndex, int* InChecks) : Index(InIndex), Checks(InChecks) {}
  bool operator!=(const Cursor& Rhs) const
  {
    ++*Checks;
    return Index != Rhs.Index;
  }

private:
  int Index;
  int* Checks;
};

int main()
{
  int Value = 1;
  int Checks = 0;
  Color A(1, 2), B(1, 2), C(1, 3);
  Handle H(&Value), Empty(nullptr);
  Callable Bound(true), Unbound(false);
  Null N1, N2;
  Cursor First(0, &Checks), Second(1, &Checks);
  bool Ok = A == B && A != C && !(A != B) && H != nullptr && Empty == nullptr && !(Empty != nullptr) && Bound != nullptr &&
            Unbound == nullptr && N1 == N2 && !(N1 != N2) && First != Second && !(First != First);
  return Ok && Checks == 2 ? 0 : 1;
}
