// cppinput: an alias declaration's attributes stand after its name (Unreal's ExternalMutex.h:
// `using FExternalMutex [[deprecated("Use TIntrusiveMutex.")]] = TExternalMutex<...>;`)
template<typename T>
struct Mutex
{
  T state;
};
using OldMutex [[deprecated("use Mutex")]] = Mutex<int>;
using Plain [[maybe_unused]] = int;

int main()
{
  Mutex<int> m{0};
  return m.state;
}
