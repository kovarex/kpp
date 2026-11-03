/* For interop/c_import_typedef_of_bool_keyword.kpp: the functions, without the header's typedef. */
#define SPEC_C_SOURCE
#include "c_import_typedef_of_bool_keyword.h"

static const bool values[3] = {true, false, true};

const bool* bool_flags(void)
{
  return values;
}

bool bool_flag(int i)
{
  return values[i];
}
