#include "Codegen/Emitter.hpp"
#ifdef _WIN32
// SEH wrapper: catches stack overflow that C++ try/catch can't.
// Returns 0 on success, 1 on stack overflow.
static int callWithStackGuard(void(*fn)(void*), void* arg)
{
  __try { fn(arg); return 0; }
  __except(GetExceptionCode() == EXCEPTION_STACK_OVERFLOW ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
  { _resetstkoflw(); return 1; }
}
#endif

// Object model of the v0 code generator:
// - Scalars are SSA values. Class objects always live in memory; a Value of class type holds the
//   object's address.
// - A class parameter is passed as a pointer to a caller-made copy that the callee owns and
//   destroys. A class return value is constructed into a hidden `sret` slot provided by the
//   caller.
// - Locals (and temporaries) of class types that need destruction are destroyed at the end of
//   their scope, in reverse order; `return`, `break` and `continue` destroy the scopes they leave.
// - Templates: a struct layout exists per instantiation type; an instantiated function is
//   emitted on first use from a worklist, after the type checker re-typed its body for the
//   concrete arguments. All declaration types are read through declType(), which applies the
//   arguments of the instantiation being emitted (or of the class whose metadata is inspected).

// The emitters and what they share are namespace codegen's (Codegen/Emitter.hpp, no longer in an
// anonymous namespace here), so that the Codegen/ files can be translation units of their own (stage 2
// of the codegen.cpp split).
using namespace codegen;
#include "Codegen/CodegenSession.inl"
