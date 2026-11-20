// cppinput: a type named in capitals before an operator function is its return type, not a macro
// (Unreal's Utf8Iterator.h: `UTF32CHAR operator*() const`)
typedef unsigned int UTF32CHAR;
struct Iterator
{
  const char* at;
  UTF32CHAR operator*() const
  {
    return UTF32CHAR(*at);
  }
};

int main()
{
  Iterator i{"A"};
  return *i == 65 ? 0 : 1;
}
