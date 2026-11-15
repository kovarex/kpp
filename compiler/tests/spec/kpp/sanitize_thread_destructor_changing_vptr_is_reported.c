// The snippet's handshakes between its two threads, out of ThreadSanitizer's sight: kpp instruments its own
// code alone, not a C file it links, so these flags order the threads without giving ThreadSanitizer a
// happens-before edge, and the worker's waits leave nothing in its trace. Until 2026-11-15 the snippet's threads
// met through kpp Atomics: the main thread's wait acquired the worker's last call, or the worker's spinning
// filled its trace until the call's stack was gone, and ThreadSanitizer then reports nothing (CI's linux_spec
// exited 0; on one processor every run did).
static int called, stopped;

void handshakeCalled(void)
{
  __atomic_store_n(&called, 1, __ATOMIC_RELEASE);
}

void handshakeWaitCalled(void)
{
  while (!__atomic_load_n(&called, __ATOMIC_ACQUIRE))
    ;
}

void handshakeStop(void)
{
  __atomic_store_n(&stopped, 1, __ATOMIC_RELEASE);
}

void handshakeWaitStop(void)
{
  while (!__atomic_load_n(&stopped, __ATOMIC_ACQUIRE))
    ;
}
