# UniquePtr<T>, UniqueArray<T>

## UniquePtr

Sole ownership of one object on the heap: the object is deleted (with `delete`, as T) when its
UniquePtr is destroyed, reset, assigned another object or nullptr. A UniquePtr is moved, never
copied; a moved-from UniquePtr is empty.

- Deleting through T*: when the object is of a class derived from T (it came from a
  `UniquePtr<Derived>` or `new Derived`), T's destructor must be virtual, or the program is wrong.
- Nothing is thread-safe beyond what a plain pointer is.

Members:
- `UniquePtr()`, `UniquePtr(nullptr)`: empty.
- `explicit UniquePtr(T* object)`: owns object (made with new), or nothing for null.
- move constructor and move assignment; the copy constructor and copy assignment are deleted.
- From `UniquePtr<U>&&` (constructor and assignment) when a U* converts to T* (U derives from T,
  or T is const U): takes the other's object, which is left empty. Not explicit, so a
  `UniquePtr<Derived>` is passed where a `UniquePtr<Base>` parameter is expected.
- `operator=(nullptr)`, `reset()`: delete the object; `reset(T* object)`: then own object (the
  old one is deleted after the new one is stored).
- `release()`: gives the object up without deleting it, returns it; the UniquePtr is empty.
- `get()`, `*`, `->`, `explicit operator bool`, `!`; `==`, `!=` compare the pointers (with
  another UniquePtr, with nullptr, and with a plain pointer of the same class or a base).
- `swap(other)`.
- `element_type`: T.

`makeUnique<T>(args...)`: `UniquePtr<T>(new T(args...))`, with the arguments forwarded.

## UniqueArray

What `std::unique_ptr<T[]>` converts to: sole ownership of an array of T on the heap, deleted
with its elements when the UniqueArray goes. `UniqueArray(n)` makes n default-constructed
elements; `operator[](i)`, `get()`; moved, never copied. `makeUniqueArray<T>(n)` makes one.
