// cppinput: a function the program declares by the name of a Windows macro the conversion knows (Unreal's
// FWindowsPlatformAtomics::InterlockedIncrement, declared once winnt.h's macro is undefined) is the program's function
struct Atomics
{
  static int InterlockedIncrement(int* value)
  {
    return ++*value;
  }
  static int incrementTwice(int* value)
  {
    InterlockedIncrement(value);
    return InterlockedIncrement(value);
  }
};

int main()
{
  int count = 1;
  return Atomics::InterlockedIncrement(&count) == 2 && Atomics::incrementTwice(&count) == 4 ? 0 : 1;
}
