// cppinput: wide C text in a printf format: `%ls` (and `%.*ls`, Unreal's `"%.*ls", View.Len(), View.GetData()`) of a
// wchar_t pointer, and `%s` of one known by its type (through a typedef, Unreal's TCHAR), is the pointer itself, a piece
// as a char pointer is; the converter left a `%ls` call as it was and wrote `(const char*)(x)` for `%s`, which printed
// the first character only
typedef wchar_t TCHAR;
int main()
{
  const TCHAR* name = L"abcdef";
  auto text = ssprintf("[%s|%.*ls|%ls]", name, 3, name, name + 4);
  return text == "[abcdef|abc|ef]" ? 0 : 1;
}
