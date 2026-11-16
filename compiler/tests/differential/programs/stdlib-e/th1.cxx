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
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <system_error>
#include <functional>
using Thread = std::thread;
using Mutex = std::mutex;
using RecursiveMutex = std::recursive_mutex;
using SharedMutex = std::shared_mutex;
using ConditionVariable = std::condition_variable;
using CvStatus = std::cv_status;
using SystemError = std::system_error;
using Errc = std::errc;
template <typename T> using Atomic = std::atomic<T>;
template <typename M> using LockGuard = std::lock_guard<M>;
template <typename M> using UniqueLock = std::unique_lock<M>;
template <typename M> using SharedLock = std::shared_lock<M>;
namespace thisThread = std::this_thread;
using Milliseconds = std::chrono::milliseconds;
using SteadyClock = std::chrono::steady_clock;
using std::move; using std::ref;
// Threads and locks against std, the parts cc1 left out: Thread's joinable states and ids, its
// arguments copied (a reference only through ref), join and detach on a Thread that is not
// joinable (system_error and its code); shared_mutex letting readers in together and a writer
// alone; condition_variable timeouts (wait_for and wait_until, with and without a predicate, a
// time already past) and a notify before the wait; unique_lock's owns_lock through release,
// unlock and moves, unlock without owning (operation_not_permitted); recursive_mutex depth and
// another thread's try_lock.
int errcValue(const SystemError& e) { return e.code().value(); }
void addOne(int value, int* out) { *out = value + 1; }
void addTo(int& target) { target += 5; }

int main()
{
  Thread none;
  printf("default: joinable %d, id is Id() %d\n", (int)none.joinable(), (int)(none.get_id() == Thread::id()));
  int result = 0;
  int value = 41;
  Thread t(addOne, value, &result);
  value = 0;
  printf("started: joinable %d, id differs from main %d\n", (int)t.joinable(), (int)(t.get_id() != thisThread::get_id()));
  t.join();
  printf("joined: joinable %d, result %d\n", (int)t.joinable(), result);
  try
  {
    t.join();
  }
  catch (const SystemError& e)
  {
    printf("join again throws, invalid_argument %d\n", (int)(e.code() == Errc::invalid_argument));
  }
  try
  {
    t.detach();
  }
  catch (const SystemError& e)
  {
    printf("detach throws, invalid_argument %d\n", (int)(e.code() == Errc::invalid_argument));
  }
  int target = 1;
  Thread byRef(addTo, ref(target));
  byRef.join();
  printf("ref argument: %d\n", target);
  Thread moved = Thread([]() {});
  Thread taken = move(moved);
  printf("moved: source joinable %d, target joinable %d\n", (int)moved.joinable(), (int)taken.joinable());
  taken.join();
  printf("hardware concurrency above 0: %d\n", (int)(Thread::hardware_concurrency() > 0));

  SharedMutex shared;
  Atomic<int> inside(0), most(0), writerSawReaders(0);
  Vector<Thread> readers;
  Atomic<int> started(0);
  for (int i = 0; i < 4; ++i)
    readers.push_back(Thread([&]() {
      SharedLock<SharedMutex> lock(shared);
      int now = inside.fetch_add(1) + 1;
      started.fetch_add(1);
      int seen = most.load();
      while (now > seen && !most.compare_exchange_weak(seen, now))
      {
      }
      while (started.load() < 4)
        thisThread::yield();
      thisThread::sleep_for(Milliseconds(20));
      inside.fetch_sub(1);
    }));
  for (Thread& r : readers)
    r.join();
  {
    UniqueLock<SharedMutex> writer(shared);
    writerSawReaders.store(inside.load());
    bool readerGot = true;
    Thread reader([&]() { readerGot = shared.try_lock_shared(); });
    reader.join();
    printf("shared: 4 readers inside at once %d, writer alone %d, a reader's try_lock_shared while written %d\n", (int)(most.load() == 4),
           (int)(writerSawReaders.load() == 0), (int)readerGot);
  }

  Mutex m;
  ConditionVariable cv;
  bool ready = false;
  {
    UniqueLock<Mutex> lock(m);
    auto begin = SteadyClock::now();
    bool got = cv.wait_for(lock, Milliseconds(50), [&]() { return ready; });
    auto waited = SteadyClock::now() - begin;
    printf("wait_for with a predicate never true: %d, waited at least 50 ms %d, owns after %d\n", (int)got, (int)(waited >= Milliseconds(50)),
           (int)lock.owns_lock());
    CvStatus status = cv.wait_for(lock, Milliseconds(10));
    printf("wait_for without a predicate: timeout %d\n", (int)(status == CvStatus::timeout));
    status = cv.wait_until(lock, SteadyClock::now() - Milliseconds(5));
    printf("wait_until a time past: timeout %d\n", (int)(status == CvStatus::timeout));
    ready = true;
    got = cv.wait_until(lock, SteadyClock::now() - Milliseconds(5), [&]() { return ready; });
    printf("wait_until past with a true predicate: %d\n", (int)got);
  }
  ready = false;
  Thread notifier([&]() {
    {
      LockGuard<Mutex> guard(m);
      ready = true;
    }
    cv.notify_one();
  });
  notifier.join();
  {
    UniqueLock<Mutex> lock(m);
    cv.wait(lock, [&]() { return ready; });
    printf("notify before the wait, predicate wait returns: %d\n", (int)ready);
  }

  UniqueLock<Mutex> u(m);
  printf("unique_lock owns %d\n", (int)u.owns_lock());
  u.unlock();
  printf("after unlock owns %d\n", (int)u.owns_lock());
  try
  {
    u.unlock();
  }
  catch (const SystemError& e)
  {
    printf("unlock without owning throws, operation_not_permitted %d\n", (int)(e.code() == Errc::operation_not_permitted));
  }
  u.lock();
  UniqueLock<Mutex> v = move(u);
  printf("moved: source owns %d mutex null %d, target owns %d\n", (int)u.owns_lock(), (int)(u.mutex() == nullptr), (int)v.owns_lock());
  Mutex* released = v.release();
  printf("release: owns %d, same mutex %d, still locked %d\n", (int)v.owns_lock(), (int)(released == &m), (int)!m.try_lock());
  m.unlock();

  RecursiveMutex rm;
  rm.lock();
  rm.lock();
  bool otherGot = true;
  Thread other([&]() { otherGot = rm.try_lock(); });
  other.join();
  rm.unlock();
  bool stillHeld = true;
  Thread other2([&]() { stillHeld = !rm.try_lock(); });
  other2.join();
  rm.unlock();
  bool freeNow = false;
  Thread other3([&]() {
    freeNow = rm.try_lock();
    if (freeNow)
      rm.unlock();
  });
  other3.join();
  printf("recursive: other thread's try_lock at depth 2 %d, at depth 1 held %d, after both unlocks %d\n", (int)otherGot, (int)stillHeld, (int)freeNow);
  return 0;
}
