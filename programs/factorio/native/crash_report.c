/* Bring-up only: run.cmd links it in (/INCLUDE:kpp_crash_report_install), link.cmd does not. An
   unhandled exception prints its code, address and the stack, named from the PDB the link writes,
   on stderr, and the process ends with the exception's code. The game's own crash handler hung in
   the converted game (2026-10-29: "Generating symbolized stacktrace, please wait ..." forever), so
   run.cmd turns it off with --no-crash-handler, which leaves this filter in place. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <string.h>

static void say(const char* text)
{
  DWORD written;
  WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, (DWORD)strlen(text), &written, NULL);
}

/* One frame: module!function+offset from the PDB, else module+offset. */
static void frame(HANDLE process, DWORD64 address, int index)
{
  char buffer[sizeof(SYMBOL_INFO) + 1024];
  SYMBOL_INFO* symbol = (SYMBOL_INFO*)buffer;
  memset(buffer, 0, sizeof buffer);
  symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
  symbol->MaxNameLen = 1023;
  HMODULE module = NULL;
  char path[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)address, &module))
    GetModuleFileNameA(module, path, MAX_PATH);
  const char* name = strrchr(path, '\\');
  name = name ? name + 1 : path;
  char text[1400];
  DWORD64 displacement = 0;
  if (SymFromAddr(process, address, &displacement, symbol))
    snprintf(text, sizeof text, "  %2d  %s!%s+0x%llx\n", index, name, symbol->Name, (unsigned long long)displacement);
  else
    snprintf(text, sizeof text, "  %2d  %s+0x%llx\n", index, name, (unsigned long long)(address - (DWORD64)module));
  say(text);
}

static LONG WINAPI report(EXCEPTION_POINTERS* e)
{
  EXCEPTION_RECORD* r = e->ExceptionRecord;
  char text[256];
  snprintf(text, sizeof text, "kpp crash report: exception 0x%08lx at %p", r->ExceptionCode, r->ExceptionAddress);
  say(text);
  if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
  {
    const char* what = r->ExceptionInformation[0] == 0 ? "reading" : r->ExceptionInformation[0] == 1 ? "writing" : "executing";
    snprintf(text, sizeof text, " (%s 0x%llx)", what, (unsigned long long)r->ExceptionInformation[1]);
    say(text);
  }
  say("\n");
  HANDLE process = GetCurrentProcess();
  char directory[MAX_PATH];
  GetModuleFileNameA(NULL, directory, MAX_PATH);
  char* slash = strrchr(directory, '\\');
  if (slash)
    *slash = 0;
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
  SymInitialize(process, directory, TRUE); /* the executable's directory: no symbol server */
  CONTEXT context = *e->ContextRecord;
  for (int i = 0; i < 48 && context.Rip; ++i)
  {
    frame(process, context.Rip, i);
    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, NULL);
    if (!function)
    {
      /* A leaf function: its return address is where the stack pointer points. */
      context.Rip = *(DWORD64*)context.Rsp;
      context.Rsp += 8;
      continue;
    }
    void* handlerData = NULL;
    DWORD64 establisher = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData, &establisher, NULL);
  }
  TerminateProcess(process, r->ExceptionCode);
  return EXCEPTION_EXECUTE_HANDLER;
}

static void install(void)
{
  SetUnhandledExceptionFilter(report);
}

#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU")) void (*kpp_crash_report_install)(void) = install;
