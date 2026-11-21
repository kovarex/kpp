// cppinput: a function declared through a typedef of its type (Unreal's AES.cpp: `typedef void AesFunc(...);`
// then `static AesFunc AesEncryptSW;`, defined further on) is a forward declaration, which goes
typedef void StepFunc(int* target, int amount);

static StepFunc addStep;
static StepFunc* step = &addStep;

static void addStep(int* target, int amount)
{
  *target += amount;
}

int main()
{
  int x = 1;
  step(&x, 6);
  return x == 7 ? 0 : 1;
}
