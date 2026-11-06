# Thread, mutexes, locks and condition variables

Threads of execution and what keeps their work apart (what `std::thread`, `std::this_thread`,
`std::mutex`, `std::recursive_mutex`, `std::shared_mutex`, `std::lock_guard`, `std::scoped_lock`,
`std::unique_lock`, `std::shared_lock` and `std::condition_variable` convert to), on the Windows
API. Values shared without a lock: [Atomic](atomic.md). The members have kpp names, and the
standard names as well where converted code calls them (`getId` and `get_id`).

## Thread

A Thread is joinable while it stands for a running (or finished, not yet joined) thread of
execution: from its start until `join()` or `detach()`. A default-constructed or moved-from Thread
is not joinable.

- `Thread()`: no thread.
- `explicit Thread(function, arguments...)`: starts a thread that calls `invoke(function,
  arguments...)` ([invoke](function.md)) on copies of them, made by the constructor in the calling
  thread and given to the call as rvalues. The copies are decayed: a function becomes its
  pointer, an array a pointer, a reference argument a copy of the value it refers to (pass a
  pointer, or `ref(x)`, for the thread to reach x itself). A pointer to a member function takes
  the object, or a pointer to it, as the first argument. Move-only arguments move. The copies are
  destroyed on the new thread when the call returns; what the function returns is discarded. The
  constructor returns when the thread has been created, which may be before it runs. When the
  system cannot start a thread, it throws SystemError with `Errc::resource_unavailable_try_again`.
  An exception that leaves the function ends the program.
- Moving (construction and assignment) transfers the thread; the source is then not joinable.
  Assigning to a joinable Thread ends the program. Not copyable.
- `~Thread()`: a Thread that is still joinable ends the program.
- `joinable()`: as above.
- `join()`: blocks until the thread has finished; everything it did happens before join returns.
  Then the Thread is not joinable. Throws SystemError with `Errc::invalid_argument` when it is not
  joinable, and with `Errc::resource_deadlock_would_occur` when it is the calling thread.
- `detach()`: the thread runs on without the Thread, which is then not joinable. Throws SystemError
  with `Errc::invalid_argument` when it is not joinable.
- `getId()` / `get_id()`: the thread's Id, or `Id()` when not joinable.
- `swap(other)`.
- `nativeHandle()` / `native_handle()`: the Windows HANDLE of the thread (elsewhere its `pthread_t`,
  as a pointer), or null.
- `static hardwareConcurrency()` / `hardware_concurrency()`: how many threads can run at once:
  the machine's logical processors in all its processor groups (never 0).

## Thread::Id

`Thread::Id` (also `Thread::id`) identifies a thread of execution: two Ids are equal when they
identify the same thread, and `Id()` identifies none. The Id of a finished thread may be given to
a thread started later. Ids are ordered (`<`, `<=`, `>`, `>=`, `<=>`, consistent with `==`), and
hashable (`hashOf`), so they key maps and sets. `value` is the system's id (0 for none). It is 4
bytes, so an `Atomic<Thread::Id>` is lock-free.

## thisThread

The calling thread (what `std::this_thread` converts to):
- `getId()` / `get_id()`: its Id.
- `yield()`: lets another thread run.
- `sleepFor(d)` / `sleep_for(d)`: blocks for at least d, measured on SteadyClock; a d of zero or
  less returns at once.
- `sleepUntil(t)` / `sleep_until(t)`: blocks until t has passed on t's clock.

## Mutex, RecursiveMutex, SharedMutex

A mutex is held by at most one thread at a time; `unlock()` by the holder lets the next one in,
and what the holder did before unlocking happens before what the next one does after locking. A
thread must unlock what it locked; unlocking a mutex the calling thread does not hold is not
allowed. None of them is copyable or movable; a default-constructed one is unlocked.

- `Mutex` (std::mutex): `lock()` blocks until the calling thread holds it; `tryLock()` /
  `try_lock()` takes it if it is free and says whether it did; `unlock()`. Locking it again from
  the thread that holds it blocks that thread forever.
- `RecursiveMutex` (std::recursive_mutex): as Mutex, but the holding thread may lock it again
  (and `tryLock` succeeds); it is free again after as many `unlock()` calls as locks.
- `SharedMutex` (std::shared_mutex): held by one writer (`lock`, `tryLock`, `unlock`) or by any
  number of readers at once (`lockShared` / `lock_shared`, `tryLockShared` / `try_lock_shared`,
  `unlockShared` / `unlock_shared`), never both. Not re-entrant in either mode.

## Locks

The tags `deferLock`, `tryToLock` and `adoptLock` (types `DeferLock`, `TryToLock`, `AdoptLock`;
std::defer_lock, std::try_to_lock, std::adopt_lock) tell a lock how to take its mutex: not now, by
a try, or as already held by the calling thread.

- `LockGuard<M>` (std::lock_guard): locks the mutex in its constructor, `LockGuard(m, adoptLock)`
  takes over one the thread holds; unlocks it in its destructor. Not copyable or movable.
- `ScopedLock<M...>` (std::scoped_lock): locks all the mutexes given, without deadlock whatever order
  other threads take them in (it waits for one and tries the others; when one is taken it lets go
  of all and waits for that one first), and unlocks them at its end; `ScopedLock(adoptLock, m...)`
  takes over mutexes the thread holds. `ScopedLock lock(a, b)` deduces the types (from one mutex not
  yet: templates/deduction_guide_pack_of_one_picks_partial_specialization). Of one mutex it is
  a lock guard (M needs no try_lock); of none it holds nothing. Not copyable or movable.
- `OnceFlag` (std::once_flag) and `callOnce(flag, f, args...)` / `call_once` (std::call_once): of the
  calls made with one flag, at once or one after another, one calls `f(args...)` and each returns
  after that call has finished; a call of f that throws does not count (the exception reaches its
  caller, and the next call runs f again).
- `UniqueLock<M>` (std::unique_lock): a movable owner of a mutex's lock, which it may or may not
  own at a time; its destructor unlocks only an owned lock.
  - `UniqueLock()`: no mutex. `explicit UniqueLock(m)`: locks m. `UniqueLock(m, deferLock)`: does
    not lock. `UniqueLock(m, tryToLock)`: owns the lock when `m.try_lock()` succeeds.
    `UniqueLock(m, adoptLock)`: owns m, which the thread holds.
  - Moving transfers the mutex and the ownership; assigning first unlocks an owned lock.
  - `lock()`, `tryLock()` / `try_lock()`: lock the mutex, the second only if it is free. Both
    throw SystemError: `Errc::operation_not_permitted` without a mutex,
    `Errc::resource_deadlock_would_occur` when the lock is already owned.
  - `unlock()`: throws SystemError with `Errc::operation_not_permitted` when it owns no lock.
  - `release()`: returns the mutex and lets go of it without unlocking it.
  - `mutex()`, `ownsLock()` / `owns_lock()`, `explicit operator bool` (owns the lock), `swap`.
- `SharedLock<M>` (std::shared_lock): UniqueLock's members, on the mutex's shared lock
  (`lockShared`, `tryLockShared`, `unlockShared`).

## ConditionVariable

Lets threads wait for a condition that other threads make true (std::condition_variable). A wait
takes a `UniqueLock<Mutex>` that owns its lock: it unlocks the mutex and blocks as one step (a
notify after the unlock is not missed), and locks the mutex again before it returns. A wait may
also return without a notify (a spurious wake-up), so a condition is checked again after a wait:
the forms with a predicate loop until it is true. Not copyable or movable.

- `notifyOne()` / `notify_one()`: wakes one waiting thread, if any. `notifyAll()` / `notify_all()`:
  wakes them all.
- `wait(lock)`; `wait(lock, ready)`: waits while `!ready()`.
- `waitUntil(lock, t)` / `wait_until`: waits until notified or until t has passed on t's clock;
  returns `CvStatus::timeout` when t has passed, otherwise `CvStatus::no_timeout`.
  `waitUntil(lock, t, ready)`: waits while `!ready()` and t has not passed; returns `ready()`.
- `waitFor(lock, d)` / `wait_for`, `waitFor(lock, d, ready)`: as waitUntil with the time
  `SteadyClock::now() + d`.

`CvStatus` (std::cv_status): `no_timeout`, `timeout`.

*Differences from C++*: none in behaviour. Not provided: the free deadlock-avoiding `lock(m1, m2,
...)` (ScopedLock does it), timed mutexes, and a condition variable for other locks
(std::condition_variable_any).
