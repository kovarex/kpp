# Atomic<T>

A value that threads read and write without a lock of their own: every operation on an
`Atomic<T>` happens as one indivisible step, and all of them, on every Atomic, appear in one order
that every thread agrees on (sequential consistency). An operation by one thread that reads a value
written by another also sees everything that thread did before the write.

## Atomic<T> for small T

T is any type of 1, 2, 4 or 8 bytes that is copied by copying its bytes: the integer types and
bool, `float` and `double`, enumerations, pointers, and small structs such as a time point or a
pair of 32-bit integers. Another size does not compile. The value is aligned to its size; the
operations move it as its bytes and never convert it (an `Atomic<double>` holds 0.75 exactly). All
of them are lock-free.

Members:
- `Atomic()`: the value is `T()`. `Atomic(value)`. Not copyable or movable.
- `load()`, and the conversion to T: the value. `store(value)`, and `= value`.
- `exchange(value)`: stores value, returns the value before.
- `compareExchange(expected, desired)` / `compare_exchange_strong` / `compare_exchange_weak`: when
  the value's bytes equal expected's, stores desired and returns true; otherwise returns false and
  sets expected to the value. It never fails spuriously.
- Integers: `fetchAdd(delta)` / `fetch_add`, `fetchSub(delta)` / `fetch_sub`, returning the value
  before; `++`, `--` (prefix returns the new value, postfix the old), `+=`, `-=` (returning the new
  value). The arithmetic wraps around for every integer type, signed ones too.
- Floating point: `fetchAdd`, `fetchSub`, `+=`, `-=` as for integers, each one indivisible.
- `== value`, `!= value`: comparisons of the loaded value.
- `is_lock_free()`: true.
- `wait(old)`: blocks while the value's bytes equal old's; returns at once when they differ.
  `notifyOne()` / `notify_one`, `notifyAll()` / `notify_all`: wake the threads waiting on this
  Atomic, which return when a store has changed the value (a waiter may also wake when nothing
  changed and then waits on; notifyOne may wake more than one). Waiting blocks in a lock and
  condition variable chosen by the Atomic's address, not lock-free.

## Atomic<SharedPtr<T>>

A SharedPtr that threads load and store as a whole (the reference counts of SharedPtr are atomic
already; this makes replacing which object it points to atomic too). Each operation takes a lock
held by the Atomic, so it is not lock-free; `is_lock_free()` is false.

Members: `Atomic()` (empty), `Atomic(SharedPtr<T>)`; `load()` and the conversion to SharedPtr<T>
(a copy, sharing ownership); `store(p)` and `= p`; `exchange(p)`: the SharedPtr held before;
`compareExchange(expected, desired)` / `compare_exchange_strong` / `compare_exchange_weak`: true
and desired stored when the value holds the same object with the same ownership as expected,
otherwise false and expected set to the value. An object whose last owner is replaced is deleted
after the lock is released.

The free functions on a plain SharedPtr (`atomicLoad`, `atomicStore`, `atomicExchange`) are in
sharedptr.md.

*Differences from C++*: there are no memory-order arguments (every operation is sequentially
consistent), and no Atomic of a type larger than 8 bytes other than SharedPtr.
