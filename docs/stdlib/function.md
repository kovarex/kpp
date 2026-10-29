# Function<R(Args...)>

A callable of any kind behind one type: a lambda (with or without captures), a function pointer,
or an object with `operator()`, callable with arguments of types Args and giving an R.

- The Function holds its own copy of the callable (on the heap). Copying a Function copies the
  callable; moving one moves it and leaves the source empty.
- Calling passes the arguments on as declared (by value, by reference, forwarded) and returns
  what the callable returns, converted to R (R may be void).
- Calling an empty Function throws BadFunctionCall.
- Nothing is thread-safe beyond what the callable is.

Members:
- `Function()`, `Function(nullptr)`: empty. From any callable F that can be called with Args (not
  explicit, so a lambda passes where a Function parameter is expected).
- copy and move constructors and assignments; `= nullptr`, `reset()`: empty.
- `operator()(args...)`.
- `explicit operator bool`, `!`, `== nullptr`, `!= nullptr`: whether it holds a callable.

When two Function overloads of a function could take the same lambda (`f(Function<void()>)` and
`f(Function<void(double)>)`), the one whose parameters the lambda takes is chosen.

`BadFunctionCall`: an Exception, what calling an empty Function throws.

# ReferenceWrapper<T>

A reference that is a value (what `std::reference_wrapper` converts to): it refers to a T that
must outlive it, and unlike a `T&` it can be stored in a Vector or an InitializerList, copied, and
assigned, where assigning makes it refer to the other's T (the T itself is not touched).

Members:
- `ReferenceWrapper(T& value)` (implicit): refers to value. A non-const T& takes no rvalue, so a
  `ReferenceWrapper<T>` never refers to a temporary.
- `get()`, `operator T&()` (implicit): the T it refers to.
- `operator()(args...)`: calls the T it refers to, when that is callable.
- `type`: T.

`ref(value)` and `cref(value)` make a `ReferenceWrapper<T>` and a `ReferenceWrapper<const T>`.

*Differences from C++*: T is an object type (no ReferenceWrapper to a function); a
`ReferenceWrapper<const T>` can still be made from a temporary T, which C++ refuses (the deleted
constructor waits for a compiler fix), and then refers to it only while the temporary lives.

# invoke

`invoke(f, args...)` calls f with args the way C++'s INVOKE does (what `std::invoke` converts to;
Thread runs its function through it) and returns what that call returns:
- f is a pointer to a member function of a class C: it is called on the first argument with the
  rest. The first argument is an object of C or of a class derived from C (or a reference to
  one), a `ReferenceWrapper` of one, or anything that `*` turns into one: a pointer, a UniquePtr,
  a SharedPtr.
- f is a pointer to a data member of C: the result is that member of the one argument, found the
  same way, as a reference (assignable through when the object is).
- Otherwise f is called with all the arguments, forwarded as they were given.
