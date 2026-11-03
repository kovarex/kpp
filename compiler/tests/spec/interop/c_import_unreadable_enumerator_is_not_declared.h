/* For interop/c_import_unreadable_enumerator_is_not_declared.kpp: an enum with an enumerator whose value
   the import cannot read (none written, standing for any construct the C parser does not read). */
enum Shifting
{
  SHIFT_A = 1,
  SHIFT_B = ,
  SHIFT_C
};
