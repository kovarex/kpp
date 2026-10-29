/* The inline functions of C headers that the converted game calls. The import declares them, but
   their bodies are C, which kppc does not compile, so nothing defines them for the link. Taking
   their addresses here makes clang emit each one as an ordinary function (a COMDAT the link can
   pull from factorio-native.lib), with the headers' own bodies. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <versionhelpers.h>

void* const kpp_inline_c_functions[] = {
  (void*)gai_strerrorW,       /* SocketAddress's error texts */
  (void*)IsWindows8OrGreater, /* the DX11 renderer's feature checks */
  (void*)HRESULT_FROM_WIN32,  /* SpriteLoaders' WIC errors (winerror.h) */
};
