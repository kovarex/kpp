/* For interop/c_short_int_spellings.kpp: the multi-word integer spellings in a C header, as glibc writes uint16_t. */
typedef unsigned short int u16;
struct ShortPair { short int a; unsigned short int b; signed short int c; };
static inline unsigned short int swap16(unsigned short int x) { return (unsigned short int)((x >> 8) | (x << 8)); }
static inline long int sizeLongInt(void) { return (long int)sizeof(long int); }
