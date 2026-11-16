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
  printf("errc values:");
  for (int i = 0; i < 26; ++i)
    printf(" %d", (int)conditions[i]);
  printf("\n");

  ErrorCode none;
  describe("default", none);
  matches("default matches", none);
  ErrorCode made = make_error_code(Errc::no_such_file_or_directory);
  describe("made", made);
  matches("made matches", made);
  ErrorCode generic(13, genericCategory());
  describe("generic 13", generic);
  matches("generic 13 matches", generic);
  ErrorCode system2(2, systemCategory());
  describe("system 2", system2);
  matches("system 2 matches", system2);
  ErrorCode zeroGeneric(0, genericCategory());
  describe("generic 0", zeroGeneric);
  matches("generic 0 matches", zeroGeneric);

  ErrorCode changed = made;
  changed.assign(5, systemCategory());
  describe("assigned", changed);
  changed.clear();
  describe("cleared", changed);
  ErrorCode fromGeneric = made;
  fromGeneric.clear();
  describe("cleared from generic", fromGeneric);

  printf("code ==: %d %d %d %d %d\n", (int)(made == make_error_code(Errc::no_such_file_or_directory)), (int)(made == ErrorCode(2, systemCategory())),
         (int)(none == ErrorCode()), (int)(none == zeroGeneric), (int)(made != ErrorCode(2, genericCategory())));
  printf("category ==: %d %d %d\n", (int)(made.category() == genericCategory()), (int)(none.category() == systemCategory()),
         (int)(genericCategory() == systemCategory()));
  printf("names: %s %s\n", genericCategory().name(), systemCategory().name());
  printf("errc 0: %d %d %d %d\n", (int)(none == (Errc)0), (int)(zeroGeneric == (Errc)0), (int)(made == (Errc)0), (int)(none != (Errc)0));

  int errors[46] = {1, 2, 3, 4, 5, 6, 8, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 28, 29, 30, 31, 32, 33, 36, 38, 39, 50, 53, 67, 80,
                    82, 87, 110, 112, 123, 145, 158, 161, 170, 183, 206, 267};
  for (int e : errors)
  {
    printf("system %d", e);
    matches("", ErrorCode(e, systemCategory()));
  }
  for (int e : errors)
  {
    printf("generic %d", e);
    matches("", ErrorCode(e, genericCategory()));
  }

  try
  {
    throw SystemError(made, String("prefix"));
  }
  catch (const RuntimeError& e)
  {
    const char* text = e.what();
    printf("what starts with the prefix: %d\n", (int)(strncmp(text, "prefix: ", 8) == 0));
  }
  try
  {
    throw SystemError(ErrorCode(22, genericCategory()), String("second"));
  }
  catch (const SystemError& e)
  {
    describe("code of the error", e.code());
    matches("its matches", e.code());
  }
  return 0;
}
