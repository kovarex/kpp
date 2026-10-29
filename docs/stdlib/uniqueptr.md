# UniquePtr<T, D>, UniqueArray<T>

## UniquePtr

Sole ownership of one object on the heap: the object is given to the deleter D when its
UniquePtr is destroyed, reset, assigned another object or nullptr; the deleter is never called
on null. The default deleter, `DefaultDelete<T>` (std::default_delete), deletes it with `delete`
as T. A UniquePtr is moved, never copied; a moved-from UniquePtr is empty.

- A UniquePtr is one pointer, with any deleter. The deleter is a class without state, callable
  with a T* (`struct Closer { void operator()(T* object) const; }`): each call constructs one,
  `D()(object)`.
- Deleting through T*: when the object is of a class derived from T (it came from a
  `UniquePtr<Derived>` or `new Derived`), T's destructor must be virtual, or the program is wrong.
- Nothing is thread-safe beyond what a plain pointer is.

Members:
- `UniquePtr()`, `UniquePtr(nullptr)`: empty.
- `explicit UniquePtr(T* object)`: owns object (made so that D may free it; with the default
  deleter, made with new), or nothing for null.
- move constructor and move assignment; the copy constructor and copy assignment are deleted.
- From `UniquePtr<U, E>&&` (constructor and assignment) when a U* converts to T* (U derives from
  T, or T is const U) and E converts to D: takes the other's object, which is left empty. Not
  explicit, so a `UniquePtr<Derived>` is passed where a `UniquePtr<Base>` parameter is expected.
- `operator=(nullptr)`, `reset()`: give the object to the deleter; `reset(T* object)`: then own
  object (the old one goes to the deleter after the new one is stored).
- `release()`: gives the object up without the deleter, returns it; the UniquePtr is empty.
- `get_deleter()`: a deleter, by value.
- `get()`, `*`, `->`, `explicit operator bool`, `!`; `==`, `!=` compare the pointers (with
  another UniquePtr, with nullptr, and with a plain pointer of the same class or a base).
- `swap(other)`.
- `element_type`: T; `deleter_type`: D.

*Difference from C++*: a deleter with state is not supported: there is no constructor taking a
deleter object (`UniquePtr(object, deleter)` does not compile), and `get_deleter()` returns a new
deleter rather than a reference to a stored one. For a deleter without state, which is what all of
the game's deleters are, the behavior is C++'s.

`makeUnique<T>(args...)`: `UniquePtr<T>(new T(args...))`, with the arguments forwarded.

## UniqueArray

What `std::unique_ptr<T[]>` converts to: sole ownership of an array of T on the heap, deleted
with its elements (each destroyed, the last first) when the UniqueArray goes, is reset or is
assigned another array or nullptr. Moved, never copied; a moved-from UniqueArray is empty.

One pointer, as std's (8 bytes: a class with one keeps the size it has in C++, which the game's
size static_asserts check, `sizeof(Inventory) <= 40`). Every array it holds is laid out as
`new T[n]` makes it, the ones it makes included: for a T that needs destroying, a cookie before
the elements holds n (8 bytes, 16 for a T aligned to 16), for any other T the block is the
elements. So `delete[]` destroys and frees any of them, and an array can go from `new T[n]` to a
UniqueArray and back through release().

- `UniqueArray()`, `UniqueArray(nullptr)`: empty.
- `explicit UniqueArray(n)`: makes n value-initialized elements (numbers are zero), as
  `new T[n]()`; empty for n <= 0. `makeUniqueArray<T>(n)` makes the same.
  `makeUniqueArrayForOverwrite<T>(n)` (std::make_unique_for_overwrite<T[]>) makes n elements to be
  overwritten, as `new T[n]`: default-initialized, so a type that needs no construction (numbers,
  chars) is left uninitialized rather than zeroed.
- `explicit UniqueArray(array)`: takes over an array made by `new T[n]` (or nothing for null), and
  gives it back to `delete[]`.
- `reset()`, `= nullptr`: delete the array. `reset(array)`: takes over array (made by `new T[n]`),
  then deletes the old one; resetting to the array it holds does nothing.
- `release()`: the array, now the caller's to give to `delete[]`; the UniqueArray is empty.
- `operator[](i)`, `get()`, `explicit operator bool`, `!`, `== nullptr`, `!= nullptr`, `swap(other)`.

*Difference from C++*: none in what the converted code uses. Like std's it has no size: `new T[n]`
keeps no count for a T that needs no destroying.
