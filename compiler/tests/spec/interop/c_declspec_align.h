/* For interop/c_declspec_align.kpp: a C struct aligned with __declspec(align(N)), as winnt.h's DECLSPEC_ALIGN does. */
typedef struct __declspec(align(32)) { int x; } SpecAligned;
