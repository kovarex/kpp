// cppinput: a namespace-scope object made in parentheses whose later argument is a lambda is an
// object, not a function (Unreal's IMPLEMENT_TYPE_LAYOUT: `static const FDelayedAutoRegisterHelper
// DelayedAutoRegisterHelper10(EDelayedRegisterRunPhase::ShaderTypesReady, []{ ... });`)
namespace Phase
{
  enum Kind
  {
    Ready = 3
  };
}
int registered = 0;
struct Registrar
{
  Registrar(Phase::Kind phase, void (*run)())
  {
    registered = int(phase);
    run();
  }
};
static const Registrar registrar(Phase::Ready, [] { registered *= 2; });

int main()
{
  return registered == 6 ? 0 : 1;
}
