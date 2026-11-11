/* The C side of c_header_noreturn_ends_a_path.kpp. */
#include <stdlib.h>
#include "c_header_noreturn_ends_a_path.h"

void fail_before(int code)
{
  exit(code);
}

void fail_after(int code)
{
  exit(code);
}

_Noreturn void fail_c11(int code)
{
  exit(code);
}

void fail_declspec(int code)
{
  exit(code);
}
