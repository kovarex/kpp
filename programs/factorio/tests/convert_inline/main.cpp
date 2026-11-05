#include <cstdio>
#include "Widget.hpp"

int main()
{
  Widget a(3, 4), b(3, 4);
  printf("area %d, scaled %d %d\n", a.area(), a.scaled(), a.scaled(3));
  printf("count %d, equal %d\n", Widget::count(), int(a == b));
  printf("renamed %d, as %d, in header %d\n", a.renamed(5), a.as<int>(), a.inHeader());
  return 0;
}
