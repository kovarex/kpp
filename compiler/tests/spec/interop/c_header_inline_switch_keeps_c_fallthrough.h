/* For interop/c_header_inline_switch_keeps_c_fallthrough.kpp: a header's inline function whose switch runs on
   from case to case, as C does, labels side by side and a case running on into the next. */
static __inline int specCaseRuns(int x)
{
  int r = 0;
  switch (x)
  {
    case 0:
    case 1:
      r += 1;
    case 2:
      r += 10;
      break;
    default:
      r = -1;
  }
  return r;
}
