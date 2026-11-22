// cppinput: a macro constant whose comment holds a declaration word (lz4hc.h's `#define LZ4_STREAMHC_MINSIZE  262200  /*
// static size, for inter-version compatibility */`) is a constant, as one without: the converter read the comment's
// `static` as the body's and kept the macro a TODO, its users with no such name
#define STATE_MINSIZE  64  /* static size, for inter-version compatibility */

union State
{
  char minStateSize[STATE_MINSIZE];
  int counter;
};

int main()
{
  State s;
  return sizeof(s.minStateSize) == 64 ? 0 : 1;
}
