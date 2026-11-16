#include <compare>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <initializer_list>
#include <new>
// kpp's names for battery er1 over the C++ standard library.
#include <cstring>
#include <string>
#include <system_error>
#include <stdexcept>
using String = std::string;
using ErrorCode = std::error_code;
using Errc = std::errc;
using SystemError = std::system_error;
using RuntimeError = std::runtime_error;
using std::make_error_code;
#define genericCategory std::generic_category
#define systemCategory std::system_category
// er1 (thread I, 2026-11-09): ErrorCode, Errc and SystemError against std::error_code, std::errc and
// std::system_error: the value and category of each way to make one, clear and assign, comparison
// with another code and with every Errc (the system category's mapping of each system error to a
// condition, on Windows std's table of Windows errors, elsewhere errno), the Errc values themselves.
Errc conditions[26] = {
  Errc::operation_not_permitted, Errc::no_such_file_or_directory, Errc::no_such_process, Errc::interrupted, Errc::io_error,
  Errc::resource_unavailable_try_again, Errc::illegal_byte_sequence, Errc::permission_denied, Errc::device_or_resource_busy,
  Errc::file_exists, Errc::cross_device_link, Errc::no_such_device, Errc::not_a_directory, Errc::is_a_directory,
  Errc::invalid_argument, Errc::too_many_files_open, Errc::file_too_large, Errc::no_space_on_device, Errc::read_only_file_system,
  Errc::resource_deadlock_would_occur, Errc::filename_too_long, Errc::function_not_supported, Errc::directory_not_empty,
  Errc::not_supported, Errc::operation_not_supported, Errc::timed_out};

const char* category(const ErrorCode& ec)
{
  return ec.category().name();
}

void describe(const char* label, const ErrorCode& ec)
{
  bool asBool = false;
  if (ec)
    asBool = true;
  printf("%s: value %d category %s bool %d\n", label, ec.value(), category(ec), (int)asBool);
}

void matches(const char* label, const ErrorCode& ec)
{
  printf("%s:", label);
  int count = 0;
  for (int i = 0; i < 26; ++i)
  {
    bool equal = ec == conditions[i];
    bool reversed = conditions[i] == ec;
    bool different = ec != conditions[i];
    if (equal)
    {
      printf(" %d", i);
      ++count;
    }
    if (equal != reversed || equal == different)
      printf(" (inconsistent at %d)", i);
  }
  printf(" (%d)\n", count);
}

int main()
{
  for (int e = 1; e < 20000; ++e)
  {
    ErrorCode ec(e, systemCategory());
    int found = -1;
    for (int i = 0; i < 26; ++i)
      if (ec == conditions[i])
        found = i;
    if (found >= 0)
      printf("%d %d\n", e, found);
  }
  return 0;
}
