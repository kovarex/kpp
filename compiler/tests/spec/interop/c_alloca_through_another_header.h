/* For interop/c_alloca_through_another_header.kpp: malloc.h included by another header, not imported
   itself, as SDL_stdinc.h includes it for MSVC (and MinGW-w64's stdlib.h always). The UCRT's declares
   _alloca; MinGW-w64's makes it a macro of __builtin_alloca. */
#include <malloc.h>
