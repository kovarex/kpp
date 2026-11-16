#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <system_error>
namespace filesystem = std::filesystem;
using ErrorCode = std::error_code;
using String = std::string;
template <typename T> using Vector = std::vector<T>;
void show(const char* label, const std::string& s) { printf("%s: [%s]\n", label, s.c_str()); }
void flag(const char* label, bool b) { printf("%s: %s\n", label, b ? "true" : "false"); }
void number(const char* label, long long n) { printf("%s: %lld\n", label, n); }
template <typename V> void sortStrings(V& v) { std::sort(v.begin(), v.end()); }
void appendText(std::string& to, const std::string& s) { to += s; }
// kpp's chrono names on std::chrono's, for the C++ side of the chrono batteries.
// kpp's thread and atomic names on std's, for ax1's C++ side.
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <future>
#include <memory>
#include <stdexcept>
#include <chrono>
using Thread = std::thread;
using Mutex = std::mutex;
using ConditionVariable = std::condition_variable;
template <typename T> using Atomic = std::atomic<T>;
template <typename M> using LockGuard = std::lock_guard<M>;
template <typename M> using UniqueLock = std::unique_lock<M>;
using OnceFlag = std::once_flag;
using std::call_once;
template <typename T> using Future = std::future<T>;
template <typename T> using SharedFuture = std::shared_future<T>;
template <typename T> using Promise = std::promise<T>;
using Launch = std::launch;
using std::async;
using FutureError = std::future_error;
using RuntimeError = std::runtime_error;
template <typename T> using SharedPtr = std::shared_ptr<T>;
template <typename T, typename... A> SharedPtr<T> makeShared(A&&... a) { return std::make_shared<T>(std::forward<A>(a)...); }
namespace thisThread = std::this_thread;
using Milliseconds = std::chrono::milliseconds;
using std::memory_order_relaxed;
using std::memory_order_acquire;
using std::memory_order_release;
using std::memory_order_seq_cst;
using std::atomic_thread_fence;
// ax1: atomics and threads against std, meant for release builds (an optimizer that hoists a
// load, sinks a store or reorders a plain access past an atomic shows here): the store-buffering
// litmus test with seq_cst operations and with relaxed ones between seq_cst fences (the outcome
// r1 == r2 == 0 is forbidden), message passing with release/acquire, a relaxed spin that must see
// the store that ends it, contention on every width (wrapping at 8 and 16 bits, fetch_xor/or/and
// bits, compare_exchange_weak loops, a double's fetch_add, a two-int struct's CAS), Atomic<T*>
// arithmetic on 12-byte elements, wait/notify_one/notify_all, Atomic<SharedPtr<T>> under
// contention (no object lost or freed twice), callOnce from many threads, thread_local objects
// and their destructors, futures from async and Promise (values, an exception, a broken promise),
// a ConditionVariable queue. Every count is exact, so the output is the same on every run.
struct Item
{
  int a, b, c;
};
struct TwoInts
{
  int low, high;
};
enum class Color : unsigned char
{
  red = 1,
  green = 2,
  blue = 3
};
struct Counted
{
  static Atomic<int> alive;
  int value;
  explicit Counted(int v) : value(v) { alive.fetch_add(1); }
  ~Counted() { alive.fetch_sub(1); }
};
Atomic<int> Counted::alive{0};
struct PerThread
{
  int count = 0;
  ~PerThread() { destroyed.fetch_add(1); }
  static Atomic<int> destroyed;
};
Atomic<int> PerThread::destroyed{0};
thread_local PerThread perThread;
thread_local int plainPerThread = 5;

// The rounds of a two-thread litmus test: the main thread opens round i, both threads run their
// part, then report done.
Atomic<int> go{0}, done{0};
void awaitRound(int round)
{
  while (go.load() != round)
  {
  }
}

int main()
{
  const int threads = 8;
  // store buffering, seq_cst
  {
    Atomic<int> x(0), y(0);
    int r1 = 0, r2 = 0;
    const int rounds = 50000;
    int forbidden = 0;
    go.store(0);
    done.store(0);
    Thread a([&] {
      for (int i = 1; i <= rounds; ++i)
      {
        awaitRound(i);
        x.store(1);
        r1 = y.load();
        done.fetch_add(1);
      }
    });
    Thread b([&] {
      for (int i = 1; i <= rounds; ++i)
      {
        awaitRound(i);
        y.store(1);
        r2 = x.load();
        done.fetch_add(1);
      }
    });
    for (int i = 1; i <= rounds; ++i)
    {
      x.store(0);
      y.store(0);
      go.store(i);
      while (done.load() != 2 * i)
      {
      }
      if (r1 == 0 && r2 == 0)
        ++forbidden;
    }
    a.join();
    b.join();
    printf("store buffering seq_cst: %d rounds, forbidden outcome %d times\n", rounds, forbidden);
  }
  // store buffering, relaxed operations between seq_cst fences; and plain relaxed (a sensitivity
  // check, its count printed only with AX1_SENSITIVITY set: on x86 C++ shows the outcome there)
  for (int fenced = 1; fenced >= 0; --fenced)
  {
    Atomic<int> x(0), y(0);
    int r1 = 0, r2 = 0;
    const int rounds = 50000;
    int forbidden = 0;
    go.store(0);
    done.store(0);
    Thread a([&] {
      for (int i = 1; i <= rounds; ++i)
      {
        awaitRound(i);
        x.store(1, memory_order_relaxed);
        if (fenced)
          atomic_thread_fence(memory_order_seq_cst);
        r1 = y.load(memory_order_relaxed);
        done.fetch_add(1);
      }
    });
    Thread b([&] {
      for (int i = 1; i <= rounds; ++i)
      {
        awaitRound(i);
        y.store(1, memory_order_relaxed);
        if (fenced)
          atomic_thread_fence(memory_order_seq_cst);
        r2 = x.load(memory_order_relaxed);
        done.fetch_add(1);
      }
    });
    for (int i = 1; i <= rounds; ++i)
    {
      x.store(0);
      y.store(0);
      go.store(i);
      while (done.load() != 2 * i)
      {
      }
      if (r1 == 0 && r2 == 0)
        ++forbidden;
    }
    a.join();
    b.join();
    if (fenced)
      printf("store buffering with fences: forbidden outcome %d times\n", forbidden);
    else if (getenv("AX1_SENSITIVITY"))
      fprintf(stderr, "store buffering relaxed (allowed): %d times\n", forbidden);
  }
  // message passing: plain data, then a release store; an acquire load sees the data
  {
    int data[64];
    for (int& d : data)
      d = 0;
    Atomic<int> ready(0);
    int stale = 0;
    const int rounds = 20000;
    Atomic<int> taken(0);
    Thread reader([&] {
      for (int i = 1; i <= rounds; ++i)
      {
        while (ready.load(memory_order_acquire) != i)
        {
        }
        for (int d : data)
          if (d != i)
            ++stale;
        taken.store(i, memory_order_release);
      }
    });
    for (int i = 1; i <= rounds; ++i)
    {
      for (int& d : data)
        d = i;
      ready.store(i, memory_order_release);
      while (taken.load(memory_order_acquire) != i)
      {
      }
    }
    reader.join();
    printf("message passing: %d rounds, stale reads %d\n", rounds, stale);
  }
  // a relaxed spin ends when another thread stores
  {
    Atomic<bool> stop(false);
    Atomic<int> spun(0);
    Thread spinner([&] {
      long long spins = 0;
      while (!stop.load(memory_order_relaxed))
        ++spins;
      spun.store(spins > 0 ? 1 : 0);
    });
    thisThread::sleep_for(Milliseconds(20));
    stop.store(true, memory_order_relaxed);
    spinner.join();
    printf("relaxed spin stopped, spun %d\n", spun.load());
  }
  // contention on every width
  {
    Atomic<unsigned char> u8(0);
    Atomic<short> i16(0);
    Atomic<unsigned> u32(0);
    Atomic<long long> i64(0);
    Atomic<unsigned long long> bits(0);
    Atomic<int> orBits(0), andBits(-1);
    Atomic<int> maximum(-1000);
    Atomic<double> sum(0.0);
    Atomic<TwoInts> pair(TwoInts{0, 0});
    Atomic<int> torn(0);
    Vector<Thread> pool;
    for (int t = 0; t < threads; ++t)
      pool.push_back(Thread([&, t] {
        for (int i = 0; i < 20000; ++i)
        {
          u8.fetch_add(1);
          ++u8;
          i16.fetch_sub(3);
          u32 += 7u;
          i64.fetch_add(1000000007ll);
          bits.fetch_xor(1ull << (t * 7 % 64));
          int candidate = (i * 7919 + t * 104729) % 1000003;
          int seen = maximum.load();
          while (seen < candidate && !maximum.compare_exchange_weak(seen, candidate))
          {
          }
          sum.fetch_add(0.5);
          TwoInts expected = pair.load();
          while (!pair.compare_exchange_weak(expected, TwoInts{expected.low + 1, expected.high + 1}))
          {
          }
          TwoInts now = pair.load();
          if (now.low != now.high)
            torn.fetch_add(1);
        }
        orBits.fetch_or(1 << t);
        andBits.fetch_and(~(1 << t));
      }));
    for (Thread& th : pool)
      th.join();
    int best = -1;
    for (int t = 0; t < threads; ++t)
      for (int i = 0; i < 20000; ++i)
      {
        int candidate = (i * 7919 + t * 104729) % 1000003;
        if (candidate > best)
          best = candidate;
      }
    TwoInts last = pair.load();
    printf("u8 %d (wraps), i16 %d (wraps), u32 %u, i64 %lld, xor bits %llx, or %x, and %x\n", (int)u8.load(), (int)i16.load(), u32.load(), i64.load(),
           bits.load(), orBits.load(), andBits.load());
    printf("cas max %d (expected %d), double sum %.17g, pair %d %d, torn %d\n", maximum.load(), best, sum.load(), last.low, last.high, torn.load());
  }
  // Atomic<T*> and small types
  {
    Item items[10];
    for (int i = 0; i < 10; ++i)
      items[i] = Item{i, i * 10, i * 100};
    Atomic<Item*> p(items);
    Item* before = p.fetch_add(3);
    printf("pointer fetch_add 3: before %d now %d\n", (int)(before - items), (int)(p.load() - items));
    p += 2;
    printf("pointer += 2: %d (b %d)\n", (int)(p.load() - items), p.load()->b);
    p -= 4;
    printf("pointer -= 4: %d\n", (int)(p.load() - items));
    ++p;
    p++;
    --p;
    printf("pointer ++ ++ --: %d\n", (int)(p.load() - items));
    before = p.fetch_sub(1);
    printf("pointer fetch_sub 1: before %d now %d (c %d)\n", (int)(before - items), (int)(p.load() - items), p.load()->c);
    Item* expected = items + 5;
    bool swapped = p.compare_exchange_strong(expected, items + 9);
    printf("pointer cas from wrong: %d, expected now %d\n", (int)swapped, (int)(expected - items));
    Atomic<Color> color(Color::red);
    Color old = color.exchange(Color::blue);
    printf("enum exchange: old %d now %d\n", (int)old, (int)color.load());
    Atomic<bool> b(false);
    int wasSet = (int)b.exchange(true);
    printf("bool exchange: %d %d, lock free %d\n", wasSet, (int)b.load(), (int)b.is_lock_free());
    Atomic<signed char> s8(127);
    s8.fetch_add(1);
    printf("i8 127 + 1: %d\n", (int)s8.load());
    Atomic<unsigned short> u16(0);
    u16.fetch_sub(1);
    printf("u16 0 - 1: %d\n", (int)u16.load());
    Atomic<float> f(0.1f);
    f.fetch_add(0.2f);
    printf("float 0.1 + 0.2: %.9g\n", (double)f.load());
  }
  // wait and notify
  {
    Atomic<int> value(0);
    Atomic<int> woken(0);
    Vector<Thread> waiters;
    for (int t = 0; t < 4; ++t)
      waiters.push_back(Thread([&] {
        value.wait(0);
        woken.fetch_add(1);
      }));
    thisThread::sleep_for(Milliseconds(20));
    printf("waiters before the store: woken %d\n", woken.load());
    value.store(1);
    value.notify_all();
    for (Thread& th : waiters)
      th.join();
    printf("after notify_all: woken %d\n", woken.load());
    value.wait(5);
    printf("wait on a different value returns at once\n");
    Atomic<int> step(0);
    int seen = 0;
    Thread consumer([&] {
      int last = 0;
      while (last < 1000)
      {
        step.wait(last);
        int now = step.load();
        if (now <= last)
          continue;
        last = now;
        ++seen;
      }
    });
    for (int i = 1; i <= 1000; ++i)
    {
      step.store(i);
      step.notify_one();
      if (i % 100 == 0)
        thisThread::sleep_for(Milliseconds(1));
    }
    consumer.join();
    printf("notify_one: consumer reached 1000 (%d)\n", (int)(seen > 0));
  }
  // Atomic<SharedPtr<T>> under contention
  {
    {
      Atomic<SharedPtr<Counted>> shared(makeShared<Counted>(0));
      Atomic<int> mismatched(0);
      Vector<Thread> pool;
      for (int t = 0; t < threads; ++t)
        pool.push_back(Thread([&, t] {
          for (int i = 0; i < 2000; ++i)
          {
            if (i % 2 == 0)
              shared.store(makeShared<Counted>(t * 10000 + i));
            else
            {
              SharedPtr<Counted> mine = shared.load();
              if (!mine || mine->value < 0)
                mismatched.fetch_add(1);
            }
            SharedPtr<Counted> expected = shared.load();
            shared.compare_exchange_strong(expected, makeShared<Counted>(expected->value));
          }
        }));
      for (Thread& th : pool)
        th.join();
      printf("atomic shared_ptr: alive %d, bad loads %d, lock free %d\n", Counted::alive.load(), mismatched.load(), (int)shared.is_lock_free());
    }
    printf("atomic shared_ptr gone: alive %d\n", Counted::alive.load());
  }
  // callOnce, thread_local, Mutex
  {
    OnceFlag once;
    Atomic<int> calls(0);
    int destroyedBefore = PerThread::destroyed.load();
    int guarded = 0;
    Mutex lock;
    Vector<Thread> pool;
    for (int t = 0; t < threads; ++t)
      pool.push_back(Thread([&] {
        call_once(once, [&] { calls.fetch_add(1); });
        for (int i = 0; i < 1000; ++i)
          ++perThread.count;
        plainPerThread += 1;
        for (int i = 0; i < 10000; ++i)
        {
          LockGuard<Mutex> hold(lock);
          ++guarded;
        }
        if (perThread.count != 1000 || plainPerThread != 6)
          printf("thread_local shared between threads\n");
      }));
    for (Thread& th : pool)
      th.join();
    printf("call_once ran %d time(s); thread_local destroyed %d; main's count %d, plain %d; guarded %d\n", calls.load(), PerThread::destroyed.load() - destroyedBefore,
           perThread.count, plainPerThread, guarded);
  }
  // futures
  {
    Vector<Future<long long>> parts;
    for (int t = 0; t < threads; ++t)
      parts.push_back(async(Launch::async, [t] {
        long long s = 0;
        for (int i = 0; i < 100000; ++i)
          s += (long long)i * (t + 1);
        return s;
      }));
    long long total = 0;
    for (Future<long long>& f : parts)
      total += f.get();
    printf("async sum %lld\n", total);
    Future<int> failing = async(Launch::async, []() -> int { throw RuntimeError("from the task"); });
    try
    {
      failing.get();
      printf("no exception\n");
    }
    catch (const RuntimeError& e)
    {
      printf("future rethrew: %s\n", e.what());
    }
    Future<int> deferred = async(Launch::deferred, [] { return 42; });
    int deferredValue = deferred.get();
    printf("deferred: %d, valid after get %d\n", deferredValue, (int)deferred.valid());
    Promise<String> promise;
    Future<String> later = promise.get_future();
    Thread setter([&] {
      thisThread::sleep_for(Milliseconds(5));
      promise.set_value(String("set by a thread"));
    });
    printf("promise: %s\n", later.get().c_str());
    setter.join();
    Future<int> orphan;
    {
      Promise<int> dropped;
      orphan = dropped.get_future();
    }
    try
    {
      orphan.get();
      printf("broken promise: no exception\n");
    }
    catch (const FutureError&)
    {
      printf("broken promise: future error\n");
    }
    Promise<int> once;
    SharedFuture<int> sharedResult(once.get_future());
    Atomic<int> agreed(0);
    Vector<Thread> readers;
    for (int t = 0; t < 4; ++t)
      readers.push_back(Thread([&, sharedResult] {
        if (sharedResult.get() == 7)
          agreed.fetch_add(1);
      }));
    once.set_value(7);
    for (Thread& th : readers)
      th.join();
    printf("shared future: %d readers agreed\n", agreed.load());
  }
  // a ConditionVariable queue
  {
    Mutex lock;
    ConditionVariable ready;
    Vector<int> queue;
    bool finished = false;
    long long consumed = 0;
    int items = 0;
    Thread consumer([&] {
      UniqueLock<Mutex> hold(lock);
      for (;;)
      {
        ready.wait(hold, [&] { return !queue.empty() || finished; });
        while (!queue.empty())
        {
          consumed += queue.back();
          queue.pop_back();
          ++items;
        }
        if (finished)
          break;
      }
    });
    Vector<Thread> producers;
    for (int t = 0; t < 4; ++t)
      producers.push_back(Thread([&, t] {
        for (int i = 1; i <= 5000; ++i)
        {
          {
            LockGuard<Mutex> hold(lock);
            queue.push_back(i + t);
          }
          ready.notify_one();
        }
      }));
    for (Thread& th : producers)
      th.join();
    {
      LockGuard<Mutex> hold(lock);
      finished = true;
    }
    ready.notify_one();
    consumer.join();
    printf("queue: %d items, sum %lld\n", items, consumed);
  }
  printf("hardware threads > 0: %d\n", (int)(Thread::hardware_concurrency() > 0));
  return 0;
}
