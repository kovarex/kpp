/* Variables a C library defines and its header declares extern (extern_c_variables.kpp). */
struct SpecParams
{
  int a;
  int b;
};
extern const struct SpecParams specTable[2];
extern int specCounter;
extern const int specScalar;
int specBump(void);
