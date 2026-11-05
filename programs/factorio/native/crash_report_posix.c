/* Bring-up only, crash_report.c's Linux and FreeBSD side: the link keeps kpp_crash_report_install (build.kpp's
   --keep-symbol, ld.lld's -u), a pointer in .init_array, so that the handlers are in place at startup as
   crash_report.c's .CRT$XCU one puts Windows' there. A crash (a segfault, a bus error, an illegal
   instruction, a floating-point fault) and abort() (a failed assertion, a stub of --stub-missing, the game's
   own aborts) print the signal and the stack's return addresses as the C library's backtrace_symbols_fd gives
   them (the executable or library and the offset, which addr2line names) on stderr, and then end the process
   by the signal's default action. Only what a signal handler may call: write, backtrace (its first call
   loads libgcc's unwinder, done at startup), signal and raise. */
#if defined(__linux__) || defined(__FreeBSD__) /* (nothing on Windows: a build.kpp that compiles every file of the directory) */
#include <stddef.h>
#include <sys/cdefs.h> /* (FreeBSD's execinfo.h needs it first) */
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>

static void crashed(int sig)
{
  static const char head[] = "kpp crash report: signal ";
  char text[16];
  int n = 0;
  char digits[12];
  int d = 0;
  for (int s = sig; d == 0 || s != 0; s /= 10)
    digits[d++] = (char)('0' + s % 10);
  while (d > 0)
    text[n++] = digits[--d];
  text[n++] = '\n';
  (void)!write(2, head, sizeof head - 1);
  (void)!write(2, text, (size_t)n);
  void* frames[64];
  int count = (int)backtrace(frames, 64); /* (glibc's takes and gives an int, FreeBSD's a size_t) */
  backtrace_symbols_fd(frames, count, 2);
  signal(sig, SIG_DFL);
  raise(sig);
}

static void install(void)
{
  void* warm[1];
  backtrace(warm, 1); /* (its first call loads the unwinder: not in a handler) */
  static const int signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
  for (unsigned i = 0; i < sizeof signals / sizeof signals[0]; ++i)
    signal(signals[i], crashed);
}

__attribute__((section(".init_array"), used)) void (*kpp_crash_report_install)(void) = install;
#endif
