// cppinput: functions declared in a body (Unreal's SharedPointer.h: `int32 UObjectTestOverload(const volatile UObject*);` and
// `int16 UObjectTestOverload(...);` in a constructor, for a sizeof's overload resolution) go before the declaration around
// them, as a kpp body declares no function
struct Base
{
};
struct Derived : Base
{
};

template <typename T>
struct Checker
{
  static int probe()
  {
    int probeOverload(const volatile Base*);
    short probeOverload(...);
    return sizeof(probeOverload((T*)nullptr));
  }
};

int main()
{
  return Checker<Derived>::probe() == sizeof(int) && Checker<int>::probe() == sizeof(short) ? 0 : 1;
}
