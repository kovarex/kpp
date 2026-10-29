/* The C library of c_class_through_varargs.kpp: reads a SpecSlot, then a SpecView, count times. */
#include <stdarg.h>
#include "c_class_through_varargs.h"

int specReadVarargs(int count, ...)
{
  va_list arguments;
  va_start(arguments, count);
  int sum = 0;
  for (int i = 0; i < count; ++i)
  {
    struct SpecSlot slot = va_arg(arguments, struct SpecSlot);
    struct SpecView view = va_arg(arguments, struct SpecView);
    sum = sum * 100 + slot.page * 10 + slot.slot + (int)view.size + (view.data[0] == 'x' ? 1000 : 0);
  }
  va_end(arguments);
  return sum;
}
