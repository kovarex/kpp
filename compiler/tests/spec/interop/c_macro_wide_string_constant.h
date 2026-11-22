/* For interop/c_macro_wide_string_constant.kpp: an object-like macro whose body is a wide string literal (evntrace.h's
   `#define KERNEL_LOGGER_NAMEW L"NT Kernel Logger"`) beside a narrow one. */
#include <stddef.h>
#define AA_LOGGER_NAMEW L"NT Kernel Logger"
#define AA_LOGGER_NAMEA "NT Kernel Logger"
