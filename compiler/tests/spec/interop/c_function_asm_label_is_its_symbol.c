/* The C side of c_function_asm_label_is_its_symbol.kpp: the symbol the header's label names, and one of
   the declared name, which a program calling it by its name links when the label is not taken. */
int scaled_by_label(int x)
{
  return x * 10;
}

int scaled(int x)
{
  return -x;
}
