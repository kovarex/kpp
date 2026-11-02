/* For interop/c_static_inline_function_declared_extern_c.kpp: functions a header defines static
   inline, as MSVC 14.44's UCRT defines time, localtime, gmtime and mktime over _time64 and the
   others (_STATIC_INLINE_UCRT_FUNCTIONS 1, its default there; 14.51's is 0, plain __inline). */
#define _STATIC_INLINE_UCRT_FUNCTIONS 1
#include <time.h>
static __inline long long specClock(long long* const out)
{
  if (out)
    *out = 1234;
  return 1234;
}
