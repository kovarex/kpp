// cppinput: a function whose later parameter starts with an attribute is a function, not an object
// made from a lambda (`[[maybe_unused]]` is no capture list; the differential suite's const programs)
int first(int a, [[maybe_unused]] int b)
{
  return a;
}

int main()
{
  return first(0, 1);
}
