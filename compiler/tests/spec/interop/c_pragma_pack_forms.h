#include <stddef.h>
#pragma pack(push, outer, 2)
typedef struct Labelled { char c; int v; } Labelled;
#pragma pack(push, 1)
typedef struct Inner { char c; int v; } Inner;
#pragma pack(4)
typedef struct Changed { char c; double d; } Changed;
#pragma pack(pop, outer)
typedef struct Natural { char c; double d; } Natural;
#define PACKED _Pragma("pack(push, 1)")
#define PACKED_END _Pragma("pack(pop)")
PACKED
typedef struct ThroughPragma { char c; int v; } ThroughPragma;
PACKED_END
#pragma pack(push, 2)
typedef union Mixed { char c[5]; int v; } Mixed;
typedef struct Holder { char c; struct { char d; int v; } nested; Natural natural; struct Later* later; } Holder;
typedef struct Bits { unsigned a : 3; char c; long long d; } Bits;
#pragma pack(pop)
typedef struct Outside { char c; Labelled inner; Changed array[2]; } Outside;
#pragma pack(push, 16)
typedef struct Wide { char c; double d; } Wide;
#pragma pack(pop)
#pragma pack(show)
#pragma pack(pop)
typedef struct AfterExtraPop { char c; double d; } AfterExtraPop;
size_t cOffsets(int which);
void cFill(Holder* h, Changed* c);
