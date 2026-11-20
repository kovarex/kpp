// cppinput: a typedef of a function type whose name is in parentheses (Unreal's MemoryLayout.h:
// `typedef void (FWriteFrozenMemoryImageFunc)(FMemoryImageWriter& Writer, ...)`) is the function type
typedef void (WriteFunc)(int* target, int value);
void write(int* target, int value)
{
  *target = value;
}

int main()
{
  WriteFunc* f = write;
  int x = 0;
  f(&x, 7);
  return x == 7 ? 0 : 1;
}
