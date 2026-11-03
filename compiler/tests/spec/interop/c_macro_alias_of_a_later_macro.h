/* For interop/c_macro_alias_of_a_later_macro.kpp: object-like macros that name another macro, which
   comes after them in name order, and is itself an alias or an expression of a third (curl's
   curl_setup_once.h: `#define EWOULDBLOCK WSAEWOULDBLOCK`, MinGW-w64's `#define WSAEWOULDBLOCK
   (WSABASEERR + 35)`). */
#define A_FIRST A_SECOND
#define A_SECOND ZZ_DERIVED
#define ZZ_BASE 10000
#define ZZ_DERIVED (ZZ_BASE + 35)
