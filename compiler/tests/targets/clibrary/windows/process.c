/* Windows' own library, for the Windows build alone (build.kpp's source(..., Platform::Windows)),
   with Windows' headers spelled as Windows code spells them: MinGW-w64's are lower case, and a
   Linux host finds them without case (source.cpp's headerCaseOverlay). Its default library is named
   as Windows code names it too, Version.lib, which a Linux host's lld looks for as libVersion.a, and
   MinGW-w64 has as libversion.a (source.cpp's libraryCaseDirectory). */
#include <WinSock2.h>
#include <Windows.h>
#pragma comment(lib, "Version.lib")

unsigned windows_process_id(void)
{
  (void)GetFileVersionInfoSizeA("kernel32.dll", 0); /* (version.dll's, so that the library is linked) */
  return (unsigned)GetCurrentProcessId();
}
