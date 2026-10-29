/* For interop/c_anonymous_member_of_named_type.kpp: MS C's anonymous member of a named struct type, as winuser.h's MONITORINFOEXW holds MONITORINFO. */
typedef struct { int x; int y; } SpecPoint2;
typedef struct { SpecPoint2; int z; } SpecPoint3;
typedef struct { char tag; SpecPoint2; } SpecTagged;
