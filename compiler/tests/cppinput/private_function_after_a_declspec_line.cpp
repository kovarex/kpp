// cppinput: a source file's static function whose `__declspec(noinline)` is on the line above it, a comment after it
// (Unreal's UnrealNames.cpp, FORCENOINLINE): its `private` goes before the __declspec
__declspec(noinline) // not inlined
static int twice(int x)
{
  return 2 * x;
}

int main()
{
  return twice(3) == 6 ? 0 : 1;
}
