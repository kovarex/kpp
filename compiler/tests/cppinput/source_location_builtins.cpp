// cppinput: clang's and MSVC's source location built-ins: as a parameter's default (`unsigned line = __builtin_LINE()`,
// Unreal's FSourceLocationImpl::current) the call site's, kpp's `caller.line()`, and elsewhere this place's, as
// __LINE__ and __FILE__ are (Unreal's UE_LOG records); the converter left them, "'__builtin_LINE' is not declared"
struct Where
{
  unsigned line = 0;
  const char* function = nullptr;
};

Where here(unsigned line = __builtin_LINE(), const char* function = __builtin_FUNCTION())
{
  Where w;
  w.line = line;
  w.function = function;
  return w;
}

int main()
{
  Where w = here();
  unsigned own = __builtin_LINE();
  return w.line == 20 && own == 21 && w.function[0] == 'm' ? 0 : 1;
}
