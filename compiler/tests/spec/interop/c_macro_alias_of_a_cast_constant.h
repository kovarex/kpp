/* For interop/c_macro_alias_of_a_cast_constant.kpp: object-like macros that name a macro casting a number to a
   typedef, blanks in its parentheses (winnt.h's `#define STATUS_ACCESS_VIOLATION ((DWORD   )0xC0000005L)`, which
   minwinbase.h's `#define EXCEPTION_ACCESS_VIOLATION STATUS_ACCESS_VIOLATION` names). */
typedef unsigned int my_dword;
#define AA_VIOLATION ZZ_STATUS_VIOLATION
#define AA_PENDING ZZ_STATUS_PENDING
#define ZZ_STATUS_VIOLATION ((my_dword   )0xC0000005L)
#define ZZ_STATUS_PENDING ((my_dword )0x00000103L)
