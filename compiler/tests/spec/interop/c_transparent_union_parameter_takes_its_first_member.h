/* For interop/c_transparent_union_parameter_takes_its_first_member.kpp: glibc's sys/socket.h read as C
   (_GNU_SOURCE): connect, bind, sendto ... take __SOCKADDR_ARG and __CONST_SOCKADDR_ARG, unions of
   pointers marked __transparent_union__, which C passes as the pointer given (its first member's ABI);
   C++ reads the same macros as `struct sockaddr *__restrict`. The attribute after the name, as glibc
   writes it, and before it. */
struct point { int x, y; };
struct other { int z; };
typedef union { struct point *__restrict __point__; struct other *__restrict __other__; } POINT_ARG __attribute__ ((__transparent_union__));
typedef union { const struct point *__restrict __point__; const struct other *__restrict __other__; } __attribute__ ((__transparent_union__)) CONST_POINT_ARG;
int sum(CONST_POINT_ARG a, int n);
int fill(POINT_ARG a);
