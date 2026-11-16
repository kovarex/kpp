#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <system_error>
namespace filesystem = std::filesystem;
using ErrorCode = std::error_code;
using String = std::string;
template <typename T> using Vector = std::vector<T>;
void show(const char* label, const std::string& s) { printf("%s: [%s]\n", label, s.c_str()); }
void flag(const char* label, bool b) { printf("%s: %s\n", label, b ? "true" : "false"); }
void number(const char* label, long long n) { printf("%s: %lld\n", label, n); }
template <typename V> void sortStrings(V& v) { std::sort(v.begin(), v.end()); }
void appendText(std::string& to, const std::string& s) { to += s; }
#include <stdexcept>
using InvalidArgument = std::invalid_argument;
using OutOfRange = std::out_of_range;
using usize = std::size_t;
using std::stoi; using std::stol; using std::stoul; using std::stoll; using std::stoull; using std::stof; using std::stod;
std::string toText(int v) { return std::to_string(v); }
std::string toText(long long v) { return std::to_string(v); }
std::string toText(unsigned long long v) { return std::to_string(v); }
std::string toText(double v) { char b[64]; snprintf(b, sizeof b, "%.17g", v); return b; }
std::string toText(float v) { char b[64]; snprintf(b, sizeof b, "%.9g", (double)v); return b; }// String to number (stoi, stol, stoul, stoll, stoull, stof, stod: value, index, errors) and number to
// text (toString of integers, format's specifications on integers and floating values).
template <typename F> void convert(const char* name, const char* text, F f)
{
  printf("%s(\"%s\"): ", name, text);
  try
  {
    usize index = 0;
    auto value = f(String(text), &index);
    printf("%s, index %d\n", toText(value).c_str(), (int)index);
  }
  catch (const InvalidArgument&)
  {
    printf("invalid\n");
  }
  catch (const OutOfRange&)
  {
    printf("out of range\n");
  }
}

int main()
{
  const char* texts[] = {"0", "42", "-42", "+7", "  12", "12abc", "abc", "", "0x1F", "077", "2147483647", "2147483648", "-2147483648", "-2147483649",
                         "4294967295", "4294967296", "-1", "9223372036854775807", "9223372036854775808", "18446744073709551615",
                         "18446744073709551616", "1e3", "1.5", " -0", "\t5"};
  for (const char* t : texts)
  {
    convert("stoi", t, [](const String& s, usize* i) { return stoi(s, i); });
    convert("stol", t, [](const String& s, usize* i) { return (long long)stol(s, i); });
    convert("stoul", t, [](const String& s, usize* i) { return (unsigned long long)stoul(s, i); });
    convert("stoll", t, [](const String& s, usize* i) { return (long long)stoll(s, i); });
    convert("stoull", t, [](const String& s, usize* i) { return (unsigned long long)stoull(s, i); });
  }
  const char* based[][2] = {{"ff", "16"}, {"0xff", "16"}, {"0XFF", "0"}, {"077", "0"}, {"0b101", "2"}, {"101", "2"}, {"z", "36"}, {"Z", "36"}, {"10", "36"},
                            {"9", "8"}, {"-ff", "16"}, {"7fffffff", "16"}, {"80000000", "16"}};
  for (auto& b : based)
  {
    int base = atoi(b[1]);
    convert("stoi base", b[0], [base](const String& s, usize* i) { return stoi(s, i, base); });
    convert("stoull base", b[0], [base](const String& s, usize* i) { return (unsigned long long)stoull(s, i, base); });
  }
  const char* floats[] = {"1.5", "-0.0", "1e10", "1e400", "-1e400", "1e-400", "inf", "-INF", "nan", "0x1p3", ".5", "5.", "1e", "  3.25xyz", "abc",
                          "3.4028235e38", "3.5e38", "1.17549435e-38", "1e-46"};
  for (const char* t : floats)
  {
    convert("stod", t, [](const String& s, usize* i) { return stod(s, i); });
    convert("stof", t, [](const String& s, usize* i) { return stof(s, i); });
  }
  long long ints[] = {0, -1, 42, 2147483647, -2147483648LL, 9223372036854775807LL, (-9223372036854775807LL - 1)};
  for (long long v : ints)
    printf("toString %s\n", toText(v).c_str());
  printf("toString u64 %s\n", toText(18446744073709551615ULL).c_str());
  return 0;
}
