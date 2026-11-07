// The functions of the Logitech LED SDK the game calls (src/LED/Logitech), as C functions for kpp:
// LogitechLEDLib.lib (libraries/Logitech/x64, on the game's link line) is C++, so its functions have
// C++'s names. overrides/src/LED/Logitech/Includes/LogitechLEDLib.kpp defines the header's
// functions on these. The enums cross as their int values.
//
// The bundled toolchain (windows-gnu, kovarex's choice (a) of 2026-11-07) links the same .lib, which
// Visual Studio built: its functions are called by their MSVC names, and what it takes from Visual
// Studio's runtime is here: operator new and delete, and the /GS stack cookie (__security_cookie,
// __security_check_cookie, __GSHandlerCheck) as vcruntime has them, a mismatch failing fast through the
// UCRT's __report_gsfailure.
#ifdef __MINGW32__
#include <stdint.h>
#include <stdlib.h>
#include <windows.h>

#define KPP_MSVC_NAME(name) __asm__(name)
namespace LogiLed
{
  enum KeyName : int {};
  enum DeviceType : int {};
}
bool LogiLedInitWithName(const char* name) KPP_MSVC_NAME("?LogiLedInitWithName@@YA_NQEBD@Z");
bool LogiLedSetTargetDevice(int targetDevice) KPP_MSVC_NAME("?LogiLedSetTargetDevice@@YA_NH@Z");
bool LogiLedSetLightingFromBitmap(unsigned char* bitmap) KPP_MSVC_NAME("?LogiLedSetLightingFromBitmap@@YA_NQEAE@Z");
bool LogiLedSetLightingForKeyWithKeyName(LogiLed::KeyName keyName, int redPercentage, int greenPercentage, int bluePercentage)
  KPP_MSVC_NAME("?LogiLedSetLightingForKeyWithKeyName@@YA_NW4KeyName@LogiLed@@HHH@Z");
bool LogiLedSetLightingForTargetZone(LogiLed::DeviceType deviceType, int zone, int redPercentage, int greenPercentage, int bluePercentage)
  KPP_MSVC_NAME("?LogiLedSetLightingForTargetZone@@YA_NW4DeviceType@LogiLed@@HHHH@Z");
void LogiLedShutdown() KPP_MSVC_NAME("?LogiLedShutdown@@YAXXZ");

extern "C"
{
  // operator new(size_t) and operator delete(void*) by their MSVC names. Visual Studio's throws
  // std::bad_alloc where no memory is left, which nothing here can throw to the .lib: it fails fast.
  void* kppMsvcOperatorNew(size_t size) KPP_MSVC_NAME("??2@YAPEAX_K@Z");
  void kppMsvcOperatorDelete(void* pointer) KPP_MSVC_NAME("??3@YAXPEAX@Z");
  void* kppMsvcOperatorNew(size_t size)
  {
    void* pointer = malloc(size ? size : 1);
    if (!pointer)
      __fastfail(FAST_FAIL_FATAL_APP_EXIT);
    return pointer;
  }
  void kppMsvcOperatorDelete(void* pointer) { free(pointer); }

  // The /GS cookie: a function of the .lib stores it xor its frame on entry and checks it on return.
  // vcruntime's default until __security_init_cookie, which sets it from the time, the process and
  // thread and the performance counter, the top 16 bits clear (gs_cookie.c, gs_support.c).
  uintptr_t __security_cookie = 0x00002B992DDFA232ull;
  __declspec(noreturn) void __report_gsfailure(uintptr_t cookie); // the UCRT's: reports and terminates

  void __security_check_cookie(uintptr_t cookie)
  {
    if (cookie != __security_cookie)
      __report_gsfailure(cookie);
  }

  // The exception handler of a /GS function without other handlers (gshandler.c): while an exception
  // unwinds through its frame, the frame's cookie is checked as its return would.
  struct KppGsHandlerData
  {
    LONG cookieOffset; // its low 3 bits: EHandler, UHandler, HasAlignment
    LONG alignedBaseOffset;
    LONG alignment;
  };
  EXCEPTION_DISPOSITION __GSHandlerCheck(PEXCEPTION_RECORD, PVOID establisherFrame, PCONTEXT, PDISPATCHER_CONTEXT dispatcherContext)
  {
    const KppGsHandlerData* data = (const KppGsHandlerData*)dispatcherContext->HandlerData;
    uintptr_t frameBase = (uintptr_t)establisherFrame;
    if (data->cookieOffset & 4)
      frameBase = (frameBase + data->alignedBaseOffset) & ~(uintptr_t)(data->alignment - 1);
    uintptr_t cookie = *(const uintptr_t*)(frameBase + (LONG)(data->cookieOffset & ~7));
    // (the cookie was xored with the frame register where the function has one, which the unwind
    // information's FrameOffset places above the establisher frame)
    const BYTE* unwindInfo = (const BYTE*)(dispatcherContext->ImageBase + dispatcherContext->FunctionEntry->UnwindData);
    uintptr_t xorValue = (uintptr_t)establisherFrame;
    if (unwindInfo[3] & 0x0f)
      xorValue += (uintptr_t)(unwindInfo[3] >> 4) * 16;
    __security_check_cookie(cookie ^ xorValue);
    return ExceptionContinueSearch;
  }
}

namespace
{
  struct KppSecurityCookieInit
  {
    KppSecurityCookieInit()
    {
      FILETIME time;
      GetSystemTimeAsFileTime(&time);
      uintptr_t cookie = ((uintptr_t)time.dwHighDateTime << 32) ^ time.dwLowDateTime;
      cookie ^= (uintptr_t)GetCurrentThreadId();
      cookie ^= (uintptr_t)GetCurrentProcessId();
      LARGE_INTEGER counter;
      QueryPerformanceCounter(&counter);
      cookie ^= ((uintptr_t)counter.LowPart << 32) ^ (uintptr_t)counter.QuadPart;
      cookie ^= (uintptr_t)&cookie;
      cookie &= 0x0000FFFFFFFFFFFFull;
      if (cookie == 0x00002B992DDFA232ull)
        ++cookie;
      __security_cookie = cookie;
    }
  } kppSecurityCookieInit;
}
#else
#include <LED/Logitech/Includes/LogitechLEDLib.h>
#endif

extern "C"
{
  bool kppLogiLedInitWithName(const char* name) { return LogiLedInitWithName(name); }
  bool kppLogiLedSetTargetDevice(int targetDevice) { return LogiLedSetTargetDevice(targetDevice); }
  bool kppLogiLedSetLightingFromBitmap(unsigned char* bitmap) { return LogiLedSetLightingFromBitmap(bitmap); }
  bool kppLogiLedSetLightingForKeyWithKeyName(int keyName, int redPercentage, int greenPercentage, int bluePercentage)
  {
    return LogiLedSetLightingForKeyWithKeyName(LogiLed::KeyName(keyName), redPercentage, greenPercentage, bluePercentage);
  }
  bool kppLogiLedSetLightingForTargetZone(int deviceType, int zone, int redPercentage, int greenPercentage, int bluePercentage)
  {
    return LogiLedSetLightingForTargetZone(LogiLed::DeviceType(deviceType), zone, redPercentage, greenPercentage, bluePercentage);
  }
  void kppLogiLedShutdown() { LogiLedShutdown(); }
}
