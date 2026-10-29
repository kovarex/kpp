/* The C library of extern_c_variables.kpp: it defines what its header declares extern. */
#include "extern_c_variables.h"

const struct SpecParams specTable[2] = { { 7, 8 }, { 20, 22 } };
int specCounter = 41;
const int specScalar = 99;

int specBump(void)
{
  return ++specCounter;
}
