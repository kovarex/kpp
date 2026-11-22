// cppinput: a macro constant defined in a top-level function's body is the rest of the file's, as C's macros are (xxhash's
// `#define XXH_SECRET_MERGEACCS_START 11` in one function, named by a later one too): a private constant before the
// function, where the converter wrote it a local constant of that function
static int first(int x)
{
#define STEP 7
  return x + STEP;
}

static int second(int x)
{
  return x * STEP;
}

int main()
{
  return first(1) == 8 && second(2) == 14 ? 0 : 1;
}
