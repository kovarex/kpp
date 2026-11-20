// cppinput: a template parameter's default type may be a function type (Unreal's TaskDelegate.h:
// `template<typename = void(), uint32 = PLATFORM_CACHE_LINE_SIZE>`)
template<typename F = void(), unsigned N = 64>
struct Delegate
{
  static constexpr unsigned size = N;
};

int main()
{
  return Delegate<>::size == 64 ? 0 : 1;
}
