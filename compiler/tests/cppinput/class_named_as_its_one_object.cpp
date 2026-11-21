// cppinput: a class and its one object of one name (Unreal's StatsSystemTypes.cpp: `static struct
// FForceInitAtBootFStats2 : public TForceInitAtBoot<FStats2Globals> {} FForceInitAtBootFStats2;`): the class goes unnamed
struct Base
{
  int value = 7;
};

static struct Booted : public Base
{
} Booted;

int main()
{
  return Booted.value == 7 ? 0 : 1;
}
