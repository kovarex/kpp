/* targets.ps1's include directory of the C import for Windows' build alone (build.kpp's
   cInclude(..., Platform::Windows)): the platform's PLATFORM_ID, as numbers' define. */
#define INCLUDED_FOR 1
/* (the C import of Windows' header spelled as Windows code spells it, which a Linux host finds without
   case: source.cpp's headerCaseOverlay) */
#include <Windows.h>
