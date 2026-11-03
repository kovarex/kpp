/* For interop/c_hresult_from_win32_is_a_function.kpp: HRESULT_FROM_WIN32's definition for the link, as
   kpp-native's inline_c.c gives it. The Windows SDK's is an inline function, which clang emits once its
   address is taken. MinGW-w64's is a macro (and under INLINE_HRESULT_FROM_WIN32 a gnu_inline function,
   which clang never emits): defined from the macro instead. */
#include <windows.h>

#ifdef __MINGW32__
#undef HRESULT_FROM_WIN32
HRESULT HRESULT_FROM_WIN32(long x)
{
  return __HRESULT_FROM_WIN32(x);
}
#else
void* const keep_hresult_from_win32[] = {(void*)HRESULT_FROM_WIN32};
#endif
