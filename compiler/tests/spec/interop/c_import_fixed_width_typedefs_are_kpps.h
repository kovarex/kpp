/* For interop/c_import_fixed_width_typedefs_are_kpps.kpp: functions that take C's fixed-width and
   size types by pointer, as SDL's SDL_StepUTF8 (size_t*) and OpenGL's glGetQueryObjecti64v (GLint64*,
   khrplatform's int64_t) do. glibc declares int64_t, ptrdiff_t, intptr_t and size_t as `long` and
   `unsigned long` on LP64 targets, MSVC as `long long`; int_fast32_t is 64 bits in glibc and 32 in
   MSVC. */
#include <stdint.h>
#include <stddef.h>
typedef int64_t spec_count;
int64_t add_int64(int64_t* value, int64_t by);
uint64_t add_uint64(uint64_t* value, uint64_t by);
size_t add_size(size_t* value, size_t by);
ptrdiff_t add_ptrdiff(ptrdiff_t* value, ptrdiff_t by);
intptr_t add_intptr(intptr_t* value, intptr_t by);
uintptr_t add_uintptr(uintptr_t* value, uintptr_t by);
spec_count add_count(spec_count* value, spec_count by);
long add_long(long* value, long by);
int fast_bytes(void);
