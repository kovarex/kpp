/* For interop/c_header_noreturn_ends_a_path.kpp: the ways a C header says that a function does not
   return, as the C libraries' headers say it: GNU's attribute before the declaration and after its
   parameters (glibc's `exit (int __status) __THROW __attribute__ ((__noreturn__))`), C11's _Noreturn
   (FreeBSD's and musl's stdlib.h) and MSVC's __declspec(noreturn) (the UCRT's exit, windows.h's
   ExitProcess). */
__attribute__((noreturn)) void fail_before(int code);
void fail_after(int code) __attribute__((__nothrow__, __noreturn__));
_Noreturn void fail_c11(int code);
#ifdef _WIN32
__declspec(noreturn) void fail_declspec(int code);
#else
__attribute__((__noreturn__)) void fail_declspec(int code);
#endif
