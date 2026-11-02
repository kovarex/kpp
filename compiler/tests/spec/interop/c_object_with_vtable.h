/* A C object whose first word points at a table of functions (as a COM interface's does), for
   c_object_with_vtable.kpp: kpp's virtual calls must find the same functions in the same slots. */
typedef struct SpecCounter SpecCounter;
typedef struct SpecCounterVtbl
{
  int (*add)(SpecCounter* self, int n);
  int (*get)(SpecCounter* self);
} SpecCounterVtbl;
struct SpecCounter
{
  const SpecCounterVtbl* vtbl;
  int value;
};
SpecCounter* specMakeCounter(int start);
int specCallAdd(SpecCounter* counter, int n);
int specCallGet(SpecCounter* counter);
