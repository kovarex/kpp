// cppinput: a C++ name `This` (Unreal's `FConsoleVariable<T>* This = (FConsoleVariable<T>*)this;`) is renamed, as kpp's
// `This` names the class the code is in
struct Counter
{
  int Value = 1;
  int Twice() const
  {
    const Counter* This = this;
    return This->Value * 2;
  }
};

int main()
{
  Counter c;
  return c.Twice() == 2 ? 0 : 1;
}
