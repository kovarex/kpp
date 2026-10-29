/* For interop/c_class_through_varargs.kpp: a C function with variable arguments that reads structs
   from them, as printf-like functions read what the game passes (MSVC x64: a struct of 1, 2, 4 or
   8 bytes travels by value, any other by the address of a copy). */
struct SpecSlot { unsigned char page; unsigned char slot; };
struct SpecView { const char* data; unsigned long long size; };
int specReadVarargs(int count, ...);
