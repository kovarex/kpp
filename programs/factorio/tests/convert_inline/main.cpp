#include <cstdio>
#include "Widget.hpp"

int main()
{
  Widget a(3, 4), b(3, 4);
  printf("area %d, scaled %d %d\n", a.area(), a.scaled(), a.scaled(3));
  printf("count %d, equal %d\n", Widget::count(), int(a == b));
  printf("renamed %d, as %d, in header %d\n", a.renamed(5), a.as<int>(), a.inHeader());
  printf("unnamed %d %d, shadowed %d %d\n", a.unnamed(1), a.unnamed(1, 2), a.shadowed(), a.shadowed(4));
  printf("summed %d\n", a.summed(std::pair<int, int>(1, 2)));
  printf("padded %d %d\n", a.padded(1), a.padded(1, 4));
  a.nothing(1);
  return 0;
}
