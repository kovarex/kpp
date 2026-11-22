// cppinput: a macro constant defined in a top-level function's body is the rest of the file's, as C's macros are (xxhash's
// `#define XXH_SECRET_MERGEACCS_START 11` in one function, named by a later one too): a private constant before the
// function, where the converter wrote it a local constant of that function; in an `extern "C" {` block (xxhash's), before
// the block, where no declaration is private
static int first(int x)
{
#define STEP 7
  return x + STEP;
}

static int second(int x)
{
  return x * STEP;
}

extern "C" {
static int third(int x)
{
#define SHIFT 3
  return x << SHIFT;
}

static int fourth(int x)
{
  return x >> SHIFT;
}
}

int main()
{
  return first(1) == 8 && second(2) == 14 && third(1) == 8 && fourth(16) == 2 ? 0 : 1;
}
