/* The C side of c_import_nullability_restrict_noreturn.kpp. */
#include <stdlib.h>
#include "c_import_nullability_restrict_noreturn.h"

int sum_values(const int * _Nonnull restrict values, int count)
{
  int sum = 0;
  for (int i = 0; i < count; ++i)
    sum += values[i];
  return sum;
}

int apply_callbacks(struct Callbacks * restrict callbacks)
{
  return callbacks->combine(3, 4) + callbacks->fallback() + callbacks->tail;
}

_Noreturn void exit_with(int code)
{
  exit(code);
}
