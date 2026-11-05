/* For interop/c_header_null_pointer_spellings.kpp: inline functions that give a pointer parameter an integer
   literal of value zero, which C takes as a null pointer constant, in the spellings C allows. */
static inline int is_set(const char* s) { return s ? 1 : 0; }
static inline int zero_ul(void) { return is_set(0UL); }
static inline int zero_lu(void) { return is_set(0lu); }
static inline int zero_ull(void) { return is_set(0ull); }
static inline int zero_hex(void) { return is_set(0x0); }
static inline int zero_octal(void) { return is_set(00); }
static inline int some(void) { return is_set("x"); }
