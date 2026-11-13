/* The C side of c_transparent_union_parameter_takes_its_first_member.kpp: the functions as their
   definitions see them, taking the pointer. */
struct point { int x, y; };

int sum(const struct point* p, int n)
{
  return p->x + p->y + n;
}

int fill(struct point* p)
{
  p->x = 4;
  p->y = 5;
  return 0;
}
