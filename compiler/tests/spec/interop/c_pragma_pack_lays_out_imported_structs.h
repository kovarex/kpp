#pragma pack(push, 4)
typedef struct Four { unsigned int id; void* pointer; int flag; } Four;
#pragma pack(pop)
#pragma pack(push, 1)
typedef struct One { char tag; int value; double d; } One;
#pragma pack(pop)
#pragma pack(2)
typedef struct Two { char c; int v; } Two;
#pragma pack()
typedef struct Plain { char c; void* p; } Plain;
