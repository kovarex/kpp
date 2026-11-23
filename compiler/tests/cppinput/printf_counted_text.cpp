// cppinput: `%.*s` (a text's first n characters, n the argument before it, Unreal's `"%.*s", View.Len(),
// View.GetData()`) is the piece std::counted(text, n); the converter left the call as it was ("`*` precision")
int main()
{
  const char* name = "abcdef";
  auto text = ssprintf("[%.*s|%s]", 3, name, name + 4);
  return text == "[abc|ef]" ? 0 : 1;
}
