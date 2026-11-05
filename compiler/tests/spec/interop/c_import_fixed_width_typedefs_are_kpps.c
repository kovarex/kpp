/* For interop/c_import_fixed_width_typedefs_are_kpps.kpp: the functions, each adding to what its
   pointer points at. */
#include "c_import_fixed_width_typedefs_are_kpps.h"

int64_t add_int64(int64_t* value, int64_t by) { return *value += by; }
uint64_t add_uint64(uint64_t* value, uint64_t by) { return *value += by; }
size_t add_size(size_t* value, size_t by) { return *value += by; }
ptrdiff_t add_ptrdiff(ptrdiff_t* value, ptrdiff_t by) { return *value += by; }
intptr_t add_intptr(intptr_t* value, intptr_t by) { return *value += by; }
uintptr_t add_uintptr(uintptr_t* value, uintptr_t by) { return *value += by; }
spec_count add_count(spec_count* value, spec_count by) { return *value += by; }
long add_long(long* value, long by) { return *value += by; }
int fast_bytes(void) { return (int)sizeof(int_fast32_t); }
