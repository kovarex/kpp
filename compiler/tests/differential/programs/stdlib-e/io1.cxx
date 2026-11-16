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
#include <fstream>
#include <string>
using InFileStream = std::ifstream;
using OutFileStream = std::ofstream;
bool getline(std::ifstream& in, std::string& line, char delimiter) { return (bool)std::getline(in, line, delimiter); }
// The file streams against std::ifstream and std::ofstream as converted code uses them: getline
// over LF, CRLF, empty lines, a last line without a newline, another delimiter, and the stream's
// state after it; get, ignore through a delimiter, read, seekg and tellg; << of text, chars and
// numbers; the bytes an OutFileStream writes (text written with "\n").
void hexOfFile(const char* name)
{
  FILE* f = fopen(name, "rb");
  int c;
  printf("  bytes:");
  while ((c = fgetc(f)) != EOF)
    printf(" %02x", c);
  printf("\n");
  fclose(f);
}
void writeRaw(const char* name, const char* bytes, int count)
{
  FILE* f = fopen(name, "wb");
  fwrite(bytes, 1, count, f);
  fclose(f);
}
void readLines(const char* label, const char* bytes, int count, char delimiter)
{
  writeRaw("io1-input.txt", bytes, count);
  InFileStream in("io1-input.txt");
  String line;
  printf("%s:", label);
  int lines = 0;
  while (getline(in, line, delimiter))
  {
    printf(" [");
    for (char ch : line)
      printf(ch == '\r' ? "\\r" : "%c", ch);
    printf("]");
    if (++lines > 20)
      break;
  }
  printf(" | eof %d good %d\n", (int)in.eof(), (int)in.good());
}

int main()
{
  readLines("LF", "one\ntwo\n\nfour\n", 14, '\n');
  readLines("no final newline", "one\ntwo", 7, '\n');
  readLines("CRLF", "one\r\ntwo\r\n", 10, '\n');
  readLines("empty file", "", 0, '\n');
  readLines("only a newline", "\n", 1, '\n');
  readLines("comma", "a,b,,c", 6, ',');
  writeRaw("io1-input.txt", "header>body\nrest", 16);
  {
    InFileStream in("io1-input.txt");
    in.ignore(1000, '>');
    int c = in.get();
    printf("ignore through '>': next %c\n", (char)c);
    in.seekg(3);
    printf("seekg 3, tellg %d, get %c\n", (int)in.tellg(), (char)in.get());
    char buffer[8] = {0};
    in.read(buffer, 5);
    printf("read 5: [%s] tellg %d\n", buffer, (int)in.tellg());
    in.ignore(100);
    printf("ignore past the end: eof %d good %d\n", (int)in.eof(), (int)in.good());
  }
  {
    OutFileStream out("io1-output.txt");
    out << "text " << 'c' << ' ' << 42 << ' ' << -7 << ' ' << 3000000000u << ' ' << 1.5 << ' ' << 0.1 << ' ' << 1e20 << ' ' << 123456789.0 << "\nsecond line\n";
  }
  printf("written:\n");
  hexOfFile("io1-output.txt");
  InFileStream missing("io1-no-such-file.txt");
  printf("missing file: open %d good %d\n", (int)missing.is_open(), (int)missing.good());
  remove("io1-input.txt");
  remove("io1-output.txt");
  return 0;
}
