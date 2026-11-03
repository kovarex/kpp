/* For interop/c_macro_cast_to_imported_typedef.kpp: object-like macros that cast a number to a typedef
   the headers declare, without parentheses around the whole body (MinGW-w64's winsock2.h: `#define
   INADDR_ANY (u_long)0x00000000`, with _bsd_types.h's `typedef unsigned long u_long;`). */
typedef unsigned short my_ushort;
typedef unsigned long my_ulong;
#define ANY_ADDRESS (my_ulong)0x00000000
#define LOOPBACK_ADDRESS (my_ulong)0x7f000001
#define SHORT_MASK (my_ushort)0xFFFF
