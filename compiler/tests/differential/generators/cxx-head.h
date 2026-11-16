// kpp's marks in the generated programs, as C++ reads them (run.py's toKpp gives kpp's)
#define IMPLICIT
#define KEXPLICIT explicit
#define KBREAK break
#define KFALLTHROUGH [[fallthrough]]
#include <typeinfo>
#include <stdexcept>
#include <exception>
#include <cstddef>
using std::uncaught_exceptions;
using std::nullptr_t;
#include <functional>
#include <utility>
using std::move;
template<class S> using Function = std::function<S>;
using std::forward;
