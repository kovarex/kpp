// cppinput: a static data member defined outside its class template, constructed in parentheses from
// named casts, is an object, not a function (Unreal's IntPoint.h: `const TIntPoint<IntType>
// TIntPoint<IntType>::NoneValue(static_cast<IntType>(INDEX_NONE), static_cast<IntType>(INDEX_NONE));`)
template<typename T>
struct Point
{
  T x, y;
  Point(T x, T y) : x(x), y(y)
  {
  }
  static const Point none;
};
template<typename T>
const Point<T> Point<T>::none(static_cast<T>(-1), static_cast<T>(-1));

int main()
{
  return Point<int>::none.x == -1 && Point<int>::none.y == -1 ? 0 : 1;
}
