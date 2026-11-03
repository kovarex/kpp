/* For interop/c_import_unreadable_member_is_error.kpp: a struct with a member the import cannot read
   (a bit-field without its width, standing for any construct the C parser does not read: it skips
   the member and goes on), and a struct that holds one. */
struct Unreadable
{
  int before;
  int during : ;
  int after;
};
typedef struct
{
  int x;
  struct Unreadable inner;
} HoldsUnreadable;
