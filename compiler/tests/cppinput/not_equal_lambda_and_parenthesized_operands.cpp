// cppinput: an iterator's operator!= with no == whose statement before its return passes a lambda with a return of its
// own (Unreal's TRangedForIterator: `ensureMsgf(...)`, its message a lambda) is made the == of its negated return, the
// lambda's return being none of the function's; an operator!= of parenthesized operands holding other operators
// (FGuid's `((X.A ^ Y.A) | (X.B ^ Y.B)) != 0`) beside its == of the same operands is dropped
template <typename F>
void check(bool condition, F message)
{
  if (!condition)
    message();
}

class Cursor
{
public:
  Cursor(int InIndex, int* InChecks) : Index(InIndex), Checks(InChecks) {}
  bool operator!=(const Cursor& Rhs) const
  {
    check(*Checks >= 0, [&]() { return "the count went below zero"; });
    ++*Checks;
    return Index != Rhs.Index;
  }

private:
  int Index;
  int* Checks;
};

struct Guid
{
  unsigned A, B;
  friend bool operator==(const Guid& X, const Guid& Y) { return ((X.A ^ Y.A) | (X.B ^ Y.B)) == 0; }
  friend bool operator!=(const Guid& X, const Guid& Y) { return ((X.A ^ Y.A) | (X.B ^ Y.B)) != 0; }
};

int main()
{
  int Checks = 0;
  Cursor First(0, &Checks), Second(1, &Checks);
  Guid G1{1, 2}, G2{1, 2}, G3{1, 3};
  bool Ok = First != Second && !(First != First) && G1 == G2 && G1 != G3 && !(G1 != G2);
  return Ok && Checks == 2 ? 0 : 1;
}
