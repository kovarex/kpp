// cppinput: a macro between a type and an operator function's name is a specifier (the game's SSE.hpp: `int32x4&
// VCALL operator+=(int32x4 right)`, VCALL its calling convention), while a name in capitals first in the declaration
// is its return type (operator_function_returning_a_caps_type)
#define VCALL
struct Vec
{
  int x;
  inline Vec& VCALL operator+=(Vec right)
  {
    x += right.x;
    return *this;
  }
  inline Vec VCALL operator-(Vec right) const
  {
    return Vec{x - right.x};
  }
};

int main()
{
  Vec a{5};
  a += Vec{2};
  return (a - Vec{3}).x == 4 ? 0 : 1;
}
