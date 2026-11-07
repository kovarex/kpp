/* The functions MinGW-w64's headers have only as static inline ones, which the converted game calls by
   name (the bundled toolchain, windows-gnu): an ordinary definition of each, with the body its header
   gives it, the UCRT's common function with the options llvm-mingw's headers pass. The headers that
   define them inline are not included: their static copies would clash with these. Visual Studio's
   UCRT exports them, so under MSVC this file is empty. */
#ifdef __MINGW32__
#include <stdarg.h>
#include <stddef.h>

typedef struct __crt_locale_pointers* kpp_locale_t;
int __stdio_common_vsscanf(unsigned long long options, const char* input, size_t length, const char* format, kpp_locale_t locale, va_list args);
int __stdio_common_vsnprintf_s(unsigned long long options, char* buffer, size_t bufferCount, size_t maxCount, const char* format, kpp_locale_t locale,
                               va_list args);
int __conio_common_vcprintf(unsigned long long options, const char* format, kpp_locale_t locale, va_list args);

/* corecrt_stdio_config.h's _CRT_INTERNAL_LOCAL_PRINTF_OPTIONS (legacy wide specifiers, standard
   rounding) and _CRT_INTERNAL_SCANF_SECURECRT, what sec_api/stdio_s.h passes */
#define KPP_PRINTF_OPTIONS (0x0004ULL | 0x0020ULL)
#define KPP_SCANF_SECURECRT 0x0001ULL

/* glad's version parse: sscanf with the secure CRT's checks (a size after each %s, %c and %[) */
int sscanf_s(const char* input, const char* format, ...)
{
  va_list args;
  va_start(args, format);
  int result = __stdio_common_vsscanf(KPP_SCANF_SECURECRT, input, (size_t)-1, format, 0, args);
  va_end(args);
  return result;
}

/* StackWalker's messages: at most maxCount characters (or as many as fit with _TRUNCATE), and the UCRT's
   invalid parameter handling where they do not fit, as Visual Studio's build has */
int _snprintf_s(char* buffer, size_t bufferCount, size_t maxCount, const char* format, ...)
{
  va_list args;
  va_start(args, format);
  int result = __stdio_common_vsnprintf_s(KPP_PRINTF_OPTIONS, buffer, bufferCount, maxCount, format, 0, args);
  va_end(args);
  return result;
}

/* minizip's buffered stream trace: to the console, not stdout */
int _cprintf(const char* format, ...)
{
  va_list args;
  va_start(args, format);
  int result = __conio_common_vcprintf(0, format, 0, args);
  va_end(args);
  return result;
}
#else
typedef int kpp_mingw_c_unused; /* (no empty translation unit) */
#endif
