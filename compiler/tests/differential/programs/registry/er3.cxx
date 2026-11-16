// er3 (thread I, 2026-11-10): the zero code against std::error_code: the spec library/error_code_zero_equals_the_errc_of_value_zero
// in C++'s names.
#include <stdio.h>
#include <system_error>
int main()
{
  std::error_code none;
  std::error_code zeroGeneric(0, std::generic_category());
  std::error_code made = std::make_error_code(std::errc::no_such_file_or_directory);
  printf("%d %d %d %d\n", (int)(none == (std::errc)0), (int)(zeroGeneric == (std::errc)0), (int)(made == (std::errc)0), (int)(none != (std::errc)0));
  printf("%d %d\n", (int)((std::errc)0 == none), (int)(none == std::errc::no_such_file_or_directory));
  made.clear();
  printf("%d %d\n", (int)(made == (std::errc)0), (int)(bool)made);
  return 0;
}
