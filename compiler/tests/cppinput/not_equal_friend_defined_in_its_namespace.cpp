// cppinput: an operator!= declared a friend in its class and defined below it, qualified by its namespace (Unreal's
// FVersePath: `friend bool operator!=(...);` and `bool UE::Core::operator!=(...) { return !(Lhs == Rhs); }`), is
// dropped with its declaration, a != b being !(a == b); the converter kept the declaration for review ("no body here")
namespace Paths
{
  class Path
  {
  public:
    explicit Path(int v) : Value(v) {}
    friend bool operator==(const Path& Lhs, const Path& Rhs);
    friend bool operator!=(const Path& Lhs, const Path& Rhs);

  private:
    int Value;
  };
}

bool Paths::operator==(const Path& Lhs, const Path& Rhs)
{
  return Lhs.Value == Rhs.Value;
}

bool Paths::operator!=(const Path& Lhs, const Path& Rhs)
{
  return !(Lhs == Rhs);
}

int main()
{
  Paths::Path a(1), b(1), c(2);
  return a != b || !(a != c) ? 1 : 0;
}
