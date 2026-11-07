/* Windows' own library, for the Windows build alone (build.kpp's source(..., Platform::Windows)),
   with Windows' headers spelled as Windows code spells them: MinGW-w64's are lower case, and a
   Linux host finds them without case (source.cpp's headerCaseOverlay). */
#include <WinSock2.h>
#include <Windows.h>

unsigned windows_process_id(void) { return (unsigned)GetCurrentProcessId(); }
