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
std::string toText(float v) { char b[64]; snprintf(b, sizeof b, "%.9g", (double)v); return b; }
// String to number at the edges cv1 left out: subnormals and the halfway points around the least
// one, long digit strings, huge exponents, hex floats, -0, inf and nan spellings, whitespace of
// every kind, doubled signs, base 0's prefixes with signs, and the index after a partial parse.
template <typename F> void convert(const char* name, const char* text, F f)
{
  printf("%s(\"", name);
  for (const char* p = text; *p; ++p)
    printf((unsigned char)*p < 0x20 ? "\\x%02x" : "%c", *p);
  printf("\"): ");
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
  String longDigits("0.");
  for (int i = 0; i < 780; ++i)
    longDigits.push_back((char)('0' + i % 10));
  String longInteger;
  for (int i = 0; i < 400; ++i)
    longInteger.push_back('9');
  const char* floats[] = {"4.9e-324", "2.4703282292062327e-324", "2.4703282292062328e-324", "2.47032822920623272e-324", "1e-324", "-1e-330",
                          "2.2250738585072011e-308", "2.2250738585072012e-308", "1.7976931348623157e308", "1.7976931348623158e308", "1.7976931348623159e308",
                          "1e-99999", "1e99999", "0e99999", "-0", "-0.0e10", "+0", "0x1.fffffffffffffp1023", "0x1p-1074", "0x1p-1075", "0X.8P1", "0x",
                          "0x1p", "infinity", "-Infinity", "infx", "nan(123)", "NAN", "-nan", "\n\t\v\f\r 7.5", " +-5", "--5", "+.5", "-.", ".", "1e+",
                          "1e-5x", "9007199254740993", "9007199254740992.5", "0.1e1", "1_000", "1,5", longDigits.c_str(), longInteger.c_str()};
  for (const char* t : floats)
  {
    convert("stod", t, [](const String& s, usize* i) { return stod(s, i); });
    convert("stof", t, [](const String& s, usize* i) { return stof(s, i); });
  }
  const char* based[][2] = {{"-0x80000000", "0"}, {"+0x7fffffff", "0"}, {" -010", "0"}, {"0x", "0"}, {"0x", "16"}, {"0X1g", "16"}, {"-0", "0"},
                            {"08", "0"}, {"0b11", "0"}, {"\n12", "10"}, {"+-1", "10"}, {"- 1", "10"}, {"1 2", "10"}, {"zz", "36"}, {"-ZZ", "36"},
                            {"99999999999999999999", "10"}, {"-9223372036854775808", "10"}, {"-9223372036854775809", "10"}};
  for (auto& b : based)
  {
    int base = atoi(b[1]);
    convert("stoi", b[0], [base](const String& s, usize* i) { return stoi(s, i, base); });
    convert("stoll", b[0], [base](const String& s, usize* i) { return (long long)stoll(s, i, base); });
    convert("stoull", b[0], [base](const String& s, usize* i) { return (unsigned long long)stoull(s, i, base); });
  }
  return 0;
}
