// cppinput: a printf format's floating conversion in a program without the trio library (Unreal Engine's
// UE_LOGF(Cat, Log, "took %6.2fms", Time), as the expander writes it through ssprintf) is the library's fixed, as
// printf's is; trio's own floating text is the game's only, which converts with `--convert-library trio`
int main()
{
  double ms = 1.5;
  auto text = ssprintf("took %.2fms", ms);
  return text == "took 1.50ms" ? 0 : 1;
}
