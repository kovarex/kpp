// cppinput: a fold whose operand is a variable template's id (Unreal's StructuredLog.h:
// `return (ValueCount<FieldArgTypes> + ...);`)
template<typename T> constexpr int width = sizeof(T);
template<typename... Ts> constexpr int total()
{
  return (width<Ts> + ...);
}

int main()
{
  return total<char, short, int>() == 7 ? 0 : 1;
}
