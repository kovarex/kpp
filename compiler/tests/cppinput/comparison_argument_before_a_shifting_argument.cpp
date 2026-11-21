// cppinput: a comparison that is one argument of a call, a later argument shifting (Unreal's UnrealNames.cpp:
// `checkf(CurrentBlock < FNameMaxBlocks, TEXT("..."), FNameMaxBlocks * BlockSizeBytes >> 20)`): kpp would read
// `CurrentBlock<FNameMaxBlocks, ...>` closed by the `>>`, so the shifting argument is written in parentheses
int pick(bool first, int second, int third)
{
  return first ? second + third : 0;
}

int main()
{
  int block = 1, maxBlocks = 4, size = 64;
  int scale = 2;
  return pick(block < maxBlocks, scale < size, maxBlocks * size >> 4) == 17 ? 0 : 1;
}
