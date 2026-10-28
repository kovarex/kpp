# SharedPtr<T>, WeakPtr<T>, EnableSharedFromThis<T>

## SharedPtr

Shared ownership of one object on the heap. Every SharedPtr that shares the object counts as one
owner; when the last owner goes (destroyed, assigned, reset or released), the object is deleted.
The owners and the weak references (below) are counted in a control block, which the first owner
creates.

- The object is deleted as the type it was created with: `SharedPtr<Base> p(SharedPtr<Derived>(new
  Derived()))` deletes a Derived even when Base has no virtual destructor.
- Two SharedPtrs share ownership only when one was made from the other (copy, conversion,
  `WeakPtr::lock`, `shared_from_this`). Two SharedPtrs each made from the same raw pointer both
  own it and delete it twice: the program is wrong.
- Thread safety: the counts change atomically. Different SharedPtr and WeakPtr objects that share
  one object may be copied, assigned and destroyed on different threads at the same time; the
  object is deleted exactly once and the control block freed exactly once. One SharedPtr object
  used from several threads at once, with at least one of them changing it, is a data race, as
  for any other object.

Members:
- `SharedPtr()`, `SharedPtr(nullptr)`: no object, `use_count()` 0.
- `explicit SharedPtr(T* object)`: owns object (made with new), or nothing for null.
- copy and move; a copy adds an owner, a move takes the other's place and leaves it empty.
- From `SharedPtr<U>` (copy or move) when a U* converts to T* (U derives from T, or T is const U):
  the same owners.
- From `UniquePtr<U>&&` when a U* converts to T*: owns what the UniquePtr owned, which is left
  empty.
- `operator=` from SharedPtr (copy, move) and nullptr; `reset()`, `release()`: give up this
  owner; `reset(T* object)`: then own object.
- `get()`, `*`, `->`, `explicit operator bool`, `!`; `==`, `!=`, `<` compare the pointers (and
  with nullptr).
- `use_count()` / `useCount()`: the number of owners, 0 when empty. With other threads changing the
  owners the value can be out of date when it is read.
- `swap(other)`.
- `element_type`: T.

`makeShared<T>(args...)`: `SharedPtr<T>(new T(args...))`, with the arguments forwarded.

## WeakPtr

A reference to an object owned by SharedPtrs that does not own it: the object can be deleted
while WeakPtrs to it remain. The control block lives while a SharedPtr or a WeakPtr uses it.

Members: `WeakPtr()`, from `SharedPtr<T>`, copy, move, assignment from all three; `reset()`;
`expired()`: true when no owner is left; `use_count()`; `lock()`: a SharedPtr that shares
ownership if the object still lives, an empty one otherwise. `lock()` is atomic: it never returns
an owner of an object that another thread is deleting.

## EnableSharedFromThis

A base class for objects that need a SharedPtr to themselves: `struct Session :
EnableSharedFromThis<Session>`. When a SharedPtr takes ownership of an object that derives from
it (`makeShared`, `SharedPtr(T*)`, `reset(T*)`, or from a UniquePtr) and the object has no living
owner yet, the object records that SharedPtr's ownership (as a weak reference). Then:

- `shared_from_this()` / `sharedFromThis()`: a SharedPtr<T> sharing that ownership; a const
  object gives SharedPtr<const T>. If no SharedPtr owns the object (it was made on the stack, with
  new and not given to a SharedPtr, or its owners are gone), it throws BadWeakPtr.
- `weak_from_this()` / `weakFromThis()`: a WeakPtr<T> to it, empty when no SharedPtr owns it.
- Copying or assigning an object copies nothing of this: the copy is not owned until a SharedPtr
  takes it.
- A class derived from one that derives from EnableSharedFromThis<Base> works the same way;
  `shared_from_this()` then gives SharedPtr<Base>.

`BadWeakPtr`: an Exception, what shared_from_this() throws.

## Atomic access

`atomicLoad(&p)`, `atomicStore(&p, q)` and `atomicExchange(&p, q)` read, write and exchange the
SharedPtr p as one indivisible step with respect to each other (and to the other calls of these
functions on p): `atomicLoad` returns a copy sharing ownership, `atomicExchange` the SharedPtr p
held before. They are not lock-free (`atomicIsLockFree(&p)` is false): each takes one of sixteen
locks, picked by p's address. A plain read or write of p is not synchronised with them. An object
whose last owner is replaced is deleted after the lock is released. For a SharedPtr that is only
ever used this way, `Atomic<SharedPtr<T>>` (atomic.md) says so in its type.
