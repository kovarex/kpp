/* The inline functions of C headers that the converted game calls. The import declares them, but
   their bodies are C, which kppc does not compile, so nothing defines them for the link. Taking
   their addresses here makes clang emit each one as an ordinary function (a COMDAT the link can
   pull from factorio-native.lib), with the headers' own bodies. MinGW-w64 (the bundled toolchain)
   has two of them in another form: IsWindows8OrGreater is static inline there, so it is renamed while
   the header defines it and an ordinary function calls it, and HRESULT_FROM_WIN32 is a macro of the
   inline __HRESULT_FROM_WIN32. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef __MINGW32__
#define IsWindows8OrGreater kppIsWindows8OrGreaterInline
#endif
#include <versionhelpers.h>
#ifdef __MINGW32__
#undef IsWindows8OrGreater
BOOL IsWindows8OrGreater(void) { return kppIsWindows8OrGreaterInline(); }
#undef HRESULT_FROM_WIN32
HRESULT HRESULT_FROM_WIN32(unsigned long x) { return __HRESULT_FROM_WIN32(x); }
#endif

void* const kpp_inline_c_functions[] = {
  (void*)gai_strerrorW,       /* SocketAddress's error texts */
  (void*)IsWindows8OrGreater, /* the DX11 renderer's feature checks */
  (void*)HRESULT_FROM_WIN32,  /* SpriteLoaders' WIC errors (winerror.h) */
};
