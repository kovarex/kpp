// cppinput: a static function in an `extern "C" {` block of a source file (Unreal's xxhash.cpp: XXH_malloc) is its
// file's own, written private outside the block, which closes before it and opens again after it
extern "C" {

static int twice(int x)
{
  return 2 * x;
}
static int thrice(int x)
{
  return 3 * x;
}

int scaled(int x)
{
  return twice(x) + thrice(x);
}

static int offset()
{
  return 1;
}

}

int main()
{
  return scaled(2) + offset() == 11 ? 0 : 1;
}
