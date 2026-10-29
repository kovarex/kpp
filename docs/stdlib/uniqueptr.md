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

- `UniqueArray()`, `UniqueArray(nullptr)`: empty.
- `explicit UniqueArray(n)`: makes n default-constructed elements. `makeUniqueArray<T>(n)` makes
  the same. `makeUniqueArrayForOverwrite<T>(n)` (std::make_unique_for_overwrite<T[]>) makes n
  elements to be overwritten: default-initialized, so a type that needs no construction (numbers,
  chars) is left uninitialized rather than zeroed.
- `explicit UniqueArray(array)`: takes over an array made by `new T[n]` (or nothing for null), and
  gives it back to `delete[]`.
- `reset()`, `= nullptr`: delete the array. `reset(array)`: takes over array (made by `new T[n]`),
  then deletes the old one; resetting to the array it holds does nothing.
- `operator[](i)`, `get()`, `explicit operator bool`, `!`, `== nullptr`, `!= nullptr`, `swap(other)`.
- `size()`: the count of an array it made (`UniqueArray(n)`, makeUniqueArray); for an array it took
  over from `new T[n]` the count is not known, and asking ends the program.

*Difference from C++*: no `release()` (no use in the converted code yet), and `size()`, which C++
does not have.
