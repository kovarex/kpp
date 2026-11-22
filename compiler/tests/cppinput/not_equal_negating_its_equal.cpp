// cppinput: an operator!= whose body negates exactly what its class's operator== returns, its parameter named alike or
// not (`return !IsEqual(Other);` beside `return IsEqual(Rhs);`, Unreal's TBigInt, TMemoryView, FCbPackage), or that
// calls the == through the class's name (`!(FConfigValue::operator==(Other))`), is dropped: kpp's `a != b` is `!(a == b)`
class BigInt
{
public:
  explicit BigInt(int InValue) : Value(InValue) {}
  bool IsEqual(const BigInt& Other) const { return Value == Other.Value; }
  bool operator==(const BigInt& Rhs) const { return IsEqual(Rhs); }
  bool operator!=(const BigInt& Other) const { return !IsEqual(Other); }

private:
  int Value;
};

class ConfigValue
{
public:
  explicit ConfigValue(int InValue) : Value(InValue) {}
  bool operator==(const ConfigValue& Other) const { return Value == Other.Value; }
  bool operator!=(const ConfigValue& Other) const { return !(ConfigValue::operator==(Other)); }

private:
  int Value;
};

int main()
{
  BigInt A(1), B(1), C(2);
  ConfigValue X(3), Y(4);
  return (A == B && A != C && X != Y && !(X != X)) ? 0 : 1;
}
