/* For interop/c_import_typedef_of_bool_keyword.kpp: functions of bool (stdbool.h's `#define bool
   _Bool`), then a typedef that the macro makes one of _Bool, which C does not allow (curl's
   curl_setup.h without HAVE_BOOL_T: `typedef enum { bool_false = 0, bool_true = 1 } bool;`), seen only
   by the import (clang compiles the .c without it). */
#include <stdbool.h>
const bool* bool_flags(void);
bool bool_flag(int i);
#ifndef SPEC_C_SOURCE
typedef enum { bool_false = 0, bool_true = 1 } bool;
#endif
