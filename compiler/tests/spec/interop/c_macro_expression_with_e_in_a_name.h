/* For interop/c_macro_expression_with_e_in_a_name.kpp: integer macros whose bodies add to another macro
   with an E in its name (MinGW-w64's psdk_inc/_wsa_errnos.h: `#define WSAEACCES (WSABASEERR + 13)`),
   beside floating ones. */
#define BASE_VALUE 10000
#define DERIVED (BASE_VALUE + 13)
#define SCALED_BASE (BASE_VALUE * 2 - 1)
#define REAL_HALF (BASE_VALUE * 0.5)
#define REAL_EXPONENT (2e3 + 1)
