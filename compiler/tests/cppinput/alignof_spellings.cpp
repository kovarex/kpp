// cppinput: MSVC's `__alignof` and GCC's `__alignof__` convert to alignof (Unreal's MinimalWindowsApi checks its
// mirrors of Windows types with `__alignof`), where they were undeclared names
#include <cstdint>

struct Mirror
{
  uint64_t Internal;
  void* Pointer;
};

struct alignas(16) Wide
{
  char Bytes[3];
};

static_assert(__alignof(Mirror) == alignof(uint64_t), "Alignment of Mirror");
static_assert(__alignof__(Wide) == 16, "Alignment of Wide");

int main()
{
  return __alignof(Mirror) == 8 && __alignof__(Wide) == 16 ? 0 : 1;
}
