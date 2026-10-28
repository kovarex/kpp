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
