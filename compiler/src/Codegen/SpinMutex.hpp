#pragma once
// Part of codegen: CodegenShared.hpp includes it inside namespace codegen.

// A mutex for the short holds that many emitters take (the shared lock, the definitions', the shards
// below): a waiter spins a while before it sleeps. With std::mutex a waiter slept at once and was
// woken by the kernel, which takes longer than the holds it waits for (2.3 us on average in the
// game's codegen), so 32 threads formed a convoy: 66.7 s waited on 3.05 s held, 12 threads blocked on
// average (docs/codegen-speed.md, step 5). The state is Drepper's: 0 free, 1 held, 2 held with a
// sleeper (std::atomic's wait and notify: WaitOnAddress).
class SpinMutex
{
  std::atomic<uint32_t> state{0};

public:
  static constexpr int spins = 1000;
  void lock()
  {
    for (int i = 0; i < spins; ++i)
    {
      uint32_t free = 0;
      if (state.load(std::memory_order_relaxed) == 0 && state.compare_exchange_weak(free, 1, std::memory_order_acquire, std::memory_order_relaxed))
        return;
#if defined(_M_X64) || defined(__x86_64__)
      _mm_pause();
#endif
    }
    while (state.exchange(2, std::memory_order_acquire) != 0)
      state.wait(2, std::memory_order_relaxed);
  }
  bool try_lock()
  {
    uint32_t free = 0;
    return state.compare_exchange_strong(free, 1, std::memory_order_acquire, std::memory_order_relaxed);
  }
  void unlock()
  {
    if (state.exchange(0, std::memory_order_release) == 2)
      state.notify_one();
  }
};
