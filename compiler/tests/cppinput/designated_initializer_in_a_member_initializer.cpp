// cppinput: a member initializer's braces may designate the members they set (Unreal's NameTypes.h:
// `: Either{.AnsiLiteral = Literal}`)
struct Either
{
  const char* ansi;
  const char* wide;
};
struct Name
{
  Either either;
  Name(const char* literal) : either{.ansi = literal}
  {
  }
};

int main()
{
  Name n("x");
  return n.either.wide == nullptr && n.either.ansi[0] == 'x' ? 0 : 1;
}
