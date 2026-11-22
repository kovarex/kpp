// cppinput: clang's and MSVC's type trait built-ins (`__is_enum(T)`, `__is_polymorphic(T)`, `__is_abstract(T)`,
// `__is_constructible(T, Args...)`, Unreal's Templates/IsEnumClass.h and IsPolymorphic.h) are kpp's traits of them
// (std::isEnum<T> ...), as std::is_enum_v is: the converter left them, "'__is_enum' is not declared"
enum class Color
{
  Red
};

struct Shape
{
  virtual ~Shape() {}
  virtual int area() const = 0;
};

struct Square : Shape
{
  int area() const override { return 4; }
};

template <typename T>
struct IsEnumClass
{
  enum { Value = __is_enum(T) };
};

template <typename T>
struct IsPolymorphic
{
  enum { Value = __is_polymorphic(T) };
};

static_assert(IsEnumClass<Color>::Value && !IsEnumClass<int>::Value, "enum");
static_assert(__is_abstract(Shape) && !__is_abstract(Square), "abstract");
static_assert(__is_constructible(Square) && !__is_constructible(Square, int), "constructible");

int main()
{
  return IsPolymorphic<Square>::Value && !IsPolymorphic<Color>::Value ? 0 : 1;
}
