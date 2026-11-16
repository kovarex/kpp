// kpp's names for battery io3 over the C++ standard library: file streams in binary mode, as kpp's are.
#include <cstdio>
#include <fstream>
#include <string>
#include <iterator>
#include <algorithm>
using String = std::string;
using std::getline;
using std::copy;
template <class C> using IstreambufIterator = std::istreambuf_iterator<C>;
#define backInserter std::back_inserter
class InFileStream : public std::ifstream
{
public:
  InFileStream() {}
  InFileStream(const char* p) : std::ifstream(p, std::ios::binary) {}
  InFileStream(InFileStream&& other) : std::ifstream(std::move(other)) {}
  void open(const char* p) { std::ifstream::open(p, std::ios::binary); }
};
class OutFileStream : public std::ofstream
{
public:
  OutFileStream() {}
  OutFileStream(const char* p) : std::ofstream(p, std::ios::binary) {}
  void open(const char* p) { std::ofstream::open(p, std::ios::binary); }
};
using std::move;
// io3 (thread I, 2026-11-09): InFileStream's fail() and clear() against std::ifstream's (binary mode):
// fail() after every kind of failure and every kind of end; clear() and what can be read after it; a
// file that grew after its end was reached, which a clear() lets the stream read.
void writeFile(const char* path, const char* data, int count)
{
  FILE* f = fopen(path, "wb");
  fwrite(data, 1, count, f);
  fclose(f);
}

void appendFile(const char* path, const char* data, int count)
{
  FILE* f = fopen(path, "ab");
  fwrite(data, 1, count, f);
  fclose(f);
}

void state(const char* label, const InFileStream& in)
{
  bool asBool = false;
  if (in)
    asBool = true;
  printf("%-36s bool %d not %d good %d eof %d fail %d\n", label, (int)asBool, (int)!in, (int)in.good(), (int)in.eof(), (int)in.fail());
}

const char* path = "io3-test.bin";

void freshStreams()
{
  InFileStream none;
  state("default constructed", none);
  InFileStream in(path);
  state("opened", in);
  InFileStream missing("io3-no-such-file.bin");
  state("missing", missing);
  missing.clear();
  state("missing: cleared", missing);
  int c = missing.get();
  printf("missing: get %d\n", c);
  state("missing: after get", missing);
  printf("missing: open %d\n", (int)missing.is_open());
}

void endOfFile()
{
  InFileStream in(path);
  String line;
  getline(in, line);
  state("first line", in);
  getline(in, line);
  state("last line, no newline", in);
  printf("[%s]\n", line.c_str());
  in.clear();
  state("cleared at the end", in);
  bool got = false;
  if (getline(in, line))
    got = true;
  printf("getline after clear %d [%s]\n", (int)got, line.c_str());
  state("getline at the end", in);
  in.clear();
  in.seekg(0);
  state("cleared and rewound", in);
  getline(in, line);
  printf("[%s]\n", line.c_str());
  state("read after rewinding", in);
}

void failedRead()
{
  InFileStream in(path);
  char buffer[100];
  in.read(buffer, 100);
  state("read past the end", in);
  in.read(buffer, 1);
  state("read after failing", in);
  in.seekg(0);
  state("seek after failing", in);
  int c = in.get();
  printf("get after failing %d\n", c);
  state("after that get", in);
  long at = in.tellg();
  printf("tellg %d\n", (int)at);
  in.clear();
  state("cleared after failing", in);
  in.seekg(0);
  state("seek after clearing", in);
  c = in.get();
  printf("get %c\n", c);
  state("after get", in);
  at = in.tellg();
  printf("tellg %d\n", (int)at);
}

void getPastTheEnd()
{
  InFileStream in(path);
  int count = 0;
  while (in.get() != -1)
    ++count;
  printf("bytes %d\n", count);
  state("get past the end", in);
  in.clear();
  state("cleared", in);
  int c = in.get();
  printf("get %d\n", c);
  state("get past the end again", in);
}

void ignoreToTheEnd()
{
  InFileStream in(path);
  in.ignore(1000);
  state("ignore to the end", in);
  in.ignore(1);
  state("ignore at the end", in);
  in.clear();
  state("cleared", in);
  in.ignore(1);
  state("ignore after clearing", in);
}

void closedStream()
{
  InFileStream in(path);
  in.close();
  state("closed", in);
  in.close();
  state("closed twice", in);
  in.clear();
  state("cleared", in);
  int c = in.get();
  printf("get %d\n", c);
  state("get with no file", in);
  in.clear();
  in.open(path);
  state("opened again", in);
  c = in.get();
  printf("get %c\n", c);
}

void openWhileOpen()
{
  InFileStream in(path);
  in.open(path);
  state("open while open", in);
  in.clear();
  state("cleared", in);
  int c = in.get();
  printf("get %c\n", c);
  printf("open %d\n", (int)in.is_open());
}

void clearOnGood()
{
  InFileStream in(path);
  int c = in.get();
  in.clear();
  state("cleared while good", in);
  int d = in.get();
  printf("got %c %c\n", c, d);
}

void aFileThatGrew()
{
  const char* grown = "io3-grown.bin";
  writeFile(grown, "ab", 2);
  InFileStream in(grown);
  String line;
  while (in.get() != -1)
  {
  }
  state("grew: at the end", in);
  appendFile(grown, "cd", 2);
  int c = in.get();
  printf("grew: no clear, get %d\n", c);
  state("grew: not cleared", in);
  in.clear();
  c = in.get();
  printf("grew: cleared, get %c\n", c);
  state("grew: after get", in);
  c = in.get();
  printf("grew: get %c\n", c);
  c = in.get();
  printf("grew: get %d\n", c);
  state("grew: at the end again", in);
}

void movedStream()
{
  InFileStream in(path);
  char buffer[100];
  in.read(buffer, 100);
  InFileStream moved(move(in));
  state("moved, failed", moved);
  moved.clear();
  moved.seekg(0);
  state("moved, cleared", moved);
  int c = moved.get();
  printf("get %c\n", c);
}

int main()
{
  writeFile(path, "first\nsecond", 12);
  freshStreams();
  endOfFile();
  failedRead();
  getPastTheEnd();
  ignoreToTheEnd();
  closedStream();
  openWhileOpen();
  clearOnGood();
  aFileThatGrew();
  movedStream();
  return 0;
}
