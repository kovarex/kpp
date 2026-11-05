#pragma once
// Part of codegen: CodegenShared.hpp includes it inside namespace codegen.

// std::recursive_mutex's semantics on a SpinMutex: the thread holding it may take it again.
class SpinRecursiveMutex
{
public:
  void lock()
  {
    std::thread::id me = std::this_thread::get_id();
    if (owner.load(std::memory_order_relaxed) != me)
    {
      mutex.lock();
      owner.store(me, std::memory_order_relaxed);
    }
    ++count;
  }
  bool try_lock()
  {
    std::thread::id me = std::this_thread::get_id();
    if (owner.load(std::memory_order_relaxed) != me)
    {
      if (!mutex.try_lock())
        return false;
      owner.store(me, std::memory_order_relaxed);
    }
    ++count;
    return true;
  }
  void unlock()
  {
    if (--count == 0)
    {
      owner.store(std::thread::id(), std::memory_order_relaxed);
      mutex.unlock();
    }
  }
private:
  SpinMutex mutex;
  std::atomic<std::thread::id> owner{};
  unsigned count = 0;
};
